// esp_overlay.cpp - consumer for libesp_core.so.
// Preloaded after libesp_core.so. Hooks eglSwapBuffers (GOT patch), calls esp_snapshot()
// once per frame on the GL thread and draws boxes + health bars with GLES2.
//
// Disable path (no restart needed):  touch /data/data/<pkg>/files/esp.disable   (rm to re-enable)
// Raw entry dump to logcat (tag esp_overlay):  touch /data/data/<pkg>/files/esp.dump
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define TAG "esp_overlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Layout recovered from esp_snapshot disassembly (build-specific, size 28):
//   +0x00..+0x0c  box in canvas px, top-left origin: left, top, right, bottom
//   +0x10         distance to camera (world units)
//   +0x14         get_Team result      (OBSERVED: slot of get_Team feeds this field)
//   +0x18         get_Health result    (OBSERVED: slot of get_Health feeds this field)
// Name mangling requires the global-namespace type name "EspEntry".
struct EspEntry {
    float left, top, right, bottom;
    float dist;
    int team;
    int health;
};
static_assert(sizeof(EspEntry) == 28, "EspEntry layout mismatch");

using SnapshotFn = bool (*)(float, float, std::vector<EspEntry>&, int&);
using SetEnabledFn = void (*)(bool);

static SnapshotFn g_snapshot = nullptr;
static SetEnabledFn g_set_enabled = nullptr;
static EGLBoolean (*g_real_swap)(EGLDisplay, EGLSurface) = nullptr;
static char g_files_dir[256] = "";

// ---------------------------------------------------------------- core resolution
static bool resolve_core() {
    if (g_snapshot && g_set_enabled) return true;
    const char* kSnap = "_Z12esp_snapshotffRNSt6__ndk16vectorI8EspEntryNS_9allocatorIS1_EEEERi";
    const char* kEn = "_Z15esp_set_enabledb";
    void* h = dlopen("libesp_core.so", RTLD_NOW | RTLD_NOLOAD);
    g_snapshot = (SnapshotFn)(h ? dlsym(h, kSnap) : dlsym(RTLD_DEFAULT, kSnap));
    g_set_enabled = (SetEnabledFn)(h ? dlsym(h, kEn) : dlsym(RTLD_DEFAULT, kEn));
    if (!g_snapshot || !g_set_enabled) {
        LOGE("libesp_core exports not found (snapshot=%p enabled=%p)", (void*)g_snapshot, (void*)g_set_enabled);
        g_snapshot = nullptr;
        g_set_enabled = nullptr;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- control files
static bool flag_file(const char* name) {
    if (!g_files_dir[0]) return false;
    char p[300];
    snprintf(p, sizeof p, "%s/%s", g_files_dir, name);
    return access(p, F_OK) == 0;
}

// ---------------------------------------------------------------- GL drawing
static GLuint g_prog = 0;
static GLint g_loc_p = -1, g_loc_res = -1, g_loc_c = -1;

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool init_gl() {
    if (g_prog) return true;
    const char* vs =
        "attribute vec2 p; uniform vec2 res;"
        "void main(){ gl_Position = vec4(p.x/res.x*2.0-1.0, 1.0-p.y/res.y*2.0, 0.0, 1.0); }";
    const char* fs = "precision mediump float; uniform vec4 c; void main(){ gl_FragColor = c; }";
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return false;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGE("program link failed");
        return false;
    }
    g_prog = p;
    g_loc_p = glGetAttribLocation(p, "p");
    g_loc_res = glGetUniformLocation(p, "res");
    g_loc_c = glGetUniformLocation(p, "c");
    return g_loc_p >= 0;
}

static void push_line(std::vector<float>& v, float x0, float y0, float x1, float y1) {
    v.insert(v.end(), {x0, y0, x1, y1});
}

static void draw_lines(const std::vector<float>& v, float r, float g, float b) {
    if (v.empty()) return;
    glUniform4f(g_loc_c, r, g, b, 1.0f);
    glVertexAttribPointer(g_loc_p, 2, GL_FLOAT, GL_FALSE, 0, v.data());
    glDrawArrays(GL_LINES, 0, (GLsizei)(v.size() / 2));
}

static void render(int w, int h, const std::vector<EspEntry>& es, int local_team) {
    if (!init_gl()) return;

    // ---- save state touched below
    GLint prog = 0, abuf = 0, vao = 0, vp[4] = {0};
    GLfloat lw = 1.0f;
    GLint attr_en = 0;
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE),
              scissor = glIsEnabled(GL_SCISSOR_TEST), stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_LINE_WIDTH, &lw);
    const bool es3 = strstr((const char*)glGetString(GL_VERSION), "OpenGL ES 3") != nullptr;
    void (*bindVAO)(GLuint) = nullptr;
    if (es3) {
        glGetIntegerv(0x85B5 /*GL_VERTEX_ARRAY_BINDING*/, &vao);
        bindVAO = (void (*)(GLuint))eglGetProcAddress("glBindVertexArray");
        if (bindVAO) bindVAO(0);
    }
    glGetVertexAttribiv(g_loc_p, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr_en);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glViewport(0, 0, w, h);
    glLineWidth(2.0f);
    glUseProgram(g_prog);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUniform2f(g_loc_res, (float)w, (float)h);
    glEnableVertexAttribArray(g_loc_p);

    std::vector<float> enemy, ally, neutral, bars;
    for (const EspEntry& e : es) {
        if (!std::isfinite(e.left) || !std::isfinite(e.right) || !std::isfinite(e.top) || !std::isfinite(e.bottom)) continue;
        std::vector<float>& dst = (local_team < 0) ? neutral : (e.team == local_team ? ally : enemy);
        push_line(dst, e.left, e.top, e.right, e.top);
        push_line(dst, e.right, e.top, e.right, e.bottom);
        push_line(dst, e.right, e.bottom, e.left, e.bottom);
        push_line(dst, e.left, e.bottom, e.left, e.top);
        // health bar, assumes max health 100 (INFERRED, adjust kMaxHealth if wrong)
        const float kMaxHealth = 100.0f;
        float frac = e.health <= 0 ? 0.0f : (e.health >= kMaxHealth ? 1.0f : e.health / kMaxHealth);
        float bx = e.left - 5.0f;
        push_line(bars, bx, e.bottom, bx, e.bottom - (e.bottom - e.top) * frac);
    }
    draw_lines(enemy, 1.0f, 0.15f, 0.15f);
    draw_lines(ally, 0.2f, 1.0f, 0.2f);
    draw_lines(neutral, 1.0f, 0.9f, 0.1f);
    draw_lines(bars, 0.1f, 0.9f, 1.0f);

    // ---- restore state
    if (!attr_en) glDisableVertexAttribArray(g_loc_p);
    if (bindVAO) bindVAO((GLuint)vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
    glUseProgram((GLuint)prog);
    glLineWidth(lw);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
}

// ---------------------------------------------------------------- frame hook
static EGLBoolean hook_swap(EGLDisplay d, EGLSurface s) {
    static unsigned frame = 0;
    static bool disabled = false;
    static std::vector<EspEntry> entries;

    if ((frame++ % 30) == 0 && resolve_core()) {
        bool want_off = flag_file("esp.disable");
        if (want_off != disabled) {
            disabled = want_off;
            g_set_enabled(!disabled);
            LOGI("esp %s", disabled ? "disabled" : "enabled");
        }
        if (frame == 1) g_set_enabled(true);
    }

    if (!disabled && g_snapshot) {
        EGLint w = 0, h = 0;
        eglQuerySurface(d, s, EGL_WIDTH, &w);
        eglQuerySurface(d, s, EGL_HEIGHT, &h);
        if (w > 0 && h > 0) {
            int local_team = -1;
            entries.clear();
            try {
                if (g_snapshot((float)w, (float)h, entries, local_team)) {
                    render(w, h, entries, local_team);
                    if ((frame % 60) == 0 && flag_file("esp.dump")) {
                        LOGI("snapshot: n=%zu localTeam=%d surface=%dx%d", entries.size(), local_team, w, h);
                        for (size_t i = 0; i < entries.size() && i < 3; i++) {
                            const EspEntry& e = entries[i];
                            LOGI("  [%zu] box=(%.1f,%.1f,%.1f,%.1f) dist=%.1f team=%d hp=%d", i, e.left, e.top,
                                 e.right, e.bottom, e.dist, e.team, e.health);
                        }
                    }
                }
            } catch (...) {
                LOGE("exception from esp_snapshot");
            }
        }
    }
    return g_real_swap(d, s);
}

// ---------------------------------------------------------------- GOT patching
static void patch_slot(void** slot) {
    if (*slot == (void*)hook_swap) return;
    uintptr_t page = (uintptr_t)slot & ~(uintptr_t)4095;
    if (mprotect((void*)page, 4096, PROT_READ | PROT_WRITE) != 0) return;
    *slot = (void*)hook_swap;
    mprotect((void*)page, 4096, PROT_READ);
}

static int phdr_cb(struct dl_phdr_info* info, size_t, void* ud) {
    const char* name = info->dlpi_name ? info->dlpi_name : "";
    if (strstr(name, "libEGL") || strstr(name, "libesp_")) return 0;
    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    if (!dyn) return 0;
    const uintptr_t bias = info->dlpi_addr;
    auto fix = [&](uintptr_t p) { return p < bias ? p + bias : p; };
    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    const ElfW(Rela)* jmprel = nullptr;
    const ElfW(Rela)* rela = nullptr;
    size_t pltsz = 0, relasz = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
            case DT_SYMTAB: symtab = (const ElfW(Sym)*)fix(dyn->d_un.d_ptr); break;
            case DT_STRTAB: strtab = (const char*)fix(dyn->d_un.d_ptr); break;
            case DT_JMPREL: jmprel = (const ElfW(Rela)*)fix(dyn->d_un.d_ptr); break;
            case DT_PLTRELSZ: pltsz = dyn->d_un.d_val; break;
            case DT_RELA: rela = (const ElfW(Rela)*)fix(dyn->d_un.d_ptr); break;
            case DT_RELASZ: relasz = dyn->d_un.d_val; break;
        }
    }
    if (!symtab || !strtab) return 0;
    auto scan = [&](const ElfW(Rela)* r, size_t sz) {
        for (size_t i = 0; r && i < sz / sizeof(ElfW(Rela)); i++) {
            unsigned type = ELF64_R_TYPE(r[i].r_info);
            if (type != 1025 /*GLOB_DAT*/ && type != 1026 /*JUMP_SLOT*/) continue;
            const char* sn = strtab + symtab[ELF64_R_SYM(r[i].r_info)].st_name;
            if (strcmp(sn, "eglSwapBuffers") != 0) continue;
            patch_slot((void**)(bias + r[i].r_offset));
            (*(int*)ud)++;
            LOGI("patched eglSwapBuffers in %s", name[0] ? name : "(main)");
        }
    };
    scan(jmprel, pltsz);
    scan(rela, relasz);
    return 0;
}

static int module_present_cb(struct dl_phdr_info* info, size_t, void* ud) {
    if (info->dlpi_name && strstr(info->dlpi_name, (const char*)ud)) return 1;
    return 0;
}

static void installer() {
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    g_real_swap = egl ? (EGLBoolean(*)(EGLDisplay, EGLSurface))dlsym(egl, "eglSwapBuffers") : nullptr;
    if (!g_real_swap) {
        LOGE("real eglSwapBuffers not found");
        return;
    }
    for (int i = 0; i < 120; i++) {  // wait up to 2 min for the engine
        if (dl_iterate_phdr(module_present_cb, (void*)"libunity.so") &&
            dl_iterate_phdr(module_present_cb, (void*)"libil2cpp.so"))
            break;
        sleep(1);
    }
    for (int round = 0; round < 10; round++) {  // rescan: modules may load late
        int n = 0;
        dl_iterate_phdr(phdr_cb, &n);
        if (round == 0 && n == 0) LOGE("no module imports eglSwapBuffers (UNKNOWN: engine may use another swap entry)");
        sleep(2);
    }
}

__attribute__((constructor)) static void overlay_init() {
    // files dir for control files, derived from the process name
    char cmd[128] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        read(fd, cmd, sizeof cmd - 1);
        close(fd);
    }
    if (cmd[0]) snprintf(g_files_dir, sizeof g_files_dir, "/data/data/%s/files", cmd);
    LOGI("overlay loaded in %s", cmd);
    std::thread(installer).detach();
}
