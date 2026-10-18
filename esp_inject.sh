#!/system/bin/sh
# esp_inject.sh - preload libesp_core.so into the target app (rooted device, run on-device).
# Mechanism: wrap.<pkg> property -> app process is started with LD_PRELOAD=<lib>.
# Usage:  sh esp_inject.sh up | down | status
# Run from Termux, or:  adb shell "su -c 'sh /data/local/tmp/esp_inject.sh up'"

PKG="FILL"   # FILL: game package id. Derive: `pm list packages | grep -i <name>`,
             # confirm with the game in foreground: `dumpsys window | grep mCurrentFocus`
LIB_NAME="libesp_core.so"          # used for map checks
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
# preload order matters: libc++_shared (if the game does not ship it) -> core -> overlay
LIBS="libc++_shared.so libesp_core.so libesp_overlay.so"
PERMISSIVE="${PERMISSIVE:-0}"   # 1 = setenforce 0 for this session (opt-in, diagnostic only)

if [ "$(id -u)" != "0" ]; then
  exec su -c "PERMISSIVE=$PERMISSIVE sh '$0' $*"
fi

die() { echo "[esp.sh] ERROR: $*" >&2; exit 1; }
log() { echo "[esp.sh] $*"; }

check_pkg() {
  [ "$PKG" != "FILL" ] || die "PKG is not set (see FILL at top of script)"
  pm path "$PKG" >/dev/null 2>&1 || die "package not installed: $PKG"
}

app_pid() { pidof "$PKG" 2>/dev/null | awk '{print $1}'; }

lib_mapped() {
  pid="$(app_pid)"
  [ -n "$pid" ] && grep -q "$LIB_NAME" "/proc/$pid/maps" 2>/dev/null
}

cmd_up() {
  check_pkg
  [ -f "$SELF_DIR/libesp_core.so" ] || die "missing $SELF_DIR/libesp_core.so"
  [ -f "$SELF_DIR/libesp_overlay.so" ] || die "missing $SELF_DIR/libesp_overlay.so (run build_overlay.sh)"

  case "$(getprop ro.product.cpu.abi)" in
    arm64*) ;;
    *) die "device ABI is not arm64; lib is aarch64-only" ;;
  esac
  pm dump "$PKG" 2>/dev/null | grep -q 'primaryCpuAbi=arm64' \
    || log "WARN: primaryCpuAbi of $PKG is not arm64 (lib will not load into a 32-bit process)"

  uid="$(stat -c %u "/data/data/$PKG")" || die "cannot stat /data/data/$PKG"
  dest="/data/data/$PKG/files"
  mkdir -p "$dest"
  preload=""
  for l in $LIBS; do
    [ -f "$SELF_DIR/$l" ] || continue
    cp "$SELF_DIR/$l" "$dest/$l" || die "copy failed: $l"
    chown "$uid:$uid" "$dest/$l"
    chmod 755 "$dest/$l"
    restorecon "$dest/$l" 2>/dev/null || true
    preload="${preload:+$preload:}$dest/$l"
  done
  rm -f "$dest/esp.disable"

  [ "$PERMISSIVE" = "1" ] && { setenforce 0 && log "SELinux set to permissive (this session)"; }

  # lib reports problems with fprintf(stderr); route native stdio to logcat
  setprop log.redirect-stdio true
  setprop "wrap.$PKG" "LD_PRELOAD=$preload"
  log "wrap.$PKG = $(getprop "wrap.$PKG")"

  am force-stop "$PKG"
  monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

  i=0
  while [ $i -lt 30 ]; do
    if lib_mapped; then
      log "OK: $LIB_NAME is mapped in pid $(app_pid)"
      grep -q libesp_overlay "/proc/$(app_pid)/maps" && log "overlay mapped" || log "WARN: overlay NOT mapped"
      log "next: touch /data/data/$PKG/files/esp.dump ; logcat -s esp_overlay   (validate entries)"
      logcat -d 2>/dev/null | grep -E 'esp_overlay|\[esp\]' | tail -n 20
      return 0
    fi
    i=$((i + 1)); sleep 1
  done
  log "FAIL: $LIB_NAME not mapped after 30s. Check: logcat -d | grep -iE 'avc|denied|linker|esp'"
  return 1
}

cmd_down() {
  check_pkg
  setprop "wrap.$PKG" ""
  setprop log.redirect-stdio false
  am force-stop "$PKG"
  for l in $LIBS; do rm -f "/data/data/$PKG/files/$l"; done
  rm -f "/data/data/$PKG/files/esp.disable" "/data/data/$PKG/files/esp.dump"
  [ "$PERMISSIVE" = "1" ] && setenforce 1
  log "removed wrap property and library; SELinux mode: $(getenforce)"
}

cmd_status() {
  check_pkg
  log "wrap.$PKG = '$(getprop "wrap.$PKG")'"
  log "SELinux: $(getenforce)"
  if lib_mapped; then log "mapped in pid $(app_pid)"; else log "not mapped"; fi
}

case "$1" in
  up) cmd_up ;;
  down) cmd_down ;;
  status) cmd_status ;;
  *) echo "usage: sh $0 up|down|status"; exit 2 ;;
esac
