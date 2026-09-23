#!/usr/bin/env bash
#
# End-to-end verification of the Windows binaries against a simulated SMC.
#
# This is the closest thing to running on a real Mac that exists without owning
# one. It drives the *shipped* executables - the same ones the release attaches -
# through the whole stack: the Win32 loader, the InpOut32 discovery, the poll
# loop, the SMC transaction framing, the key decoding, the fan bank, the
# controller, and (with a display) the window and its drawing.
#
# It exists because the port-I/O path cannot otherwise be executed on a machine
# that is not a Mac, and a code path that cannot be executed is a code path that
# is not tested. The first run of this script found a crash that would have hit
# every command of the command line tool on real hardware and nowhere else.
#
# Usage:
#   tools/fake_smc/run_e2e.sh <directory containing FanForge.exe and fanforge-cli.exe>
#
# Requires: mingw-w64 and wine. xvfb + x11-utils add the GUI checks; without them
# those checks are skipped rather than failed.
#
set -euo pipefail

BIN_DIR="${1:-}"
if [ -z "$BIN_DIR" ] || [ ! -f "$BIN_DIR/fanforge-cli.exe" ] || [ ! -f "$BIN_DIR/FanForge.exe" ]; then
    echo "usage: $0 <directory containing FanForge.exe and fanforge-cli.exe>" >&2
    exit 2
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"

cleanup() {
    pkill -f FanForge.exe 2>/dev/null || true
    pkill -f "Xvfb :77" 2>/dev/null || true
    # Wine keeps a server holding the prefix open, and it has to go first or the
    # temporary directory cannot be removed.
    if command -v wineserver >/dev/null; then
        WINEDEBUG=-all wineserver -k 2>/dev/null || true
    fi
    rm -rf "$work" 2>/dev/null || true
}
trap cleanup EXIT

CC=x86_64-w64-mingw32-gcc
command -v "$CC" >/dev/null || { echo "SKIP: $CC not available" >&2; exit 0; }
command -v wine >/dev/null || { echo "SKIP: wine not available" >&2; exit 0; }

passed=0
failed=0
step() { printf '\n== %s\n' "$1"; }
ok()   { printf '  PASS  %s\n' "$1"; passed=$((passed + 1)); }
bad()  { printf '  FAIL  %s\n' "$1"; failed=$((failed + 1)); }

# Wine maps Z: onto the filesystem root, so an absolute POSIX path can be handed
# to a Windows program by prefixing Z:.
winpath() { printf 'Z:%s' "$1"; }

# ---------------------------------------------------------------------------
step "build the fake SMC driver"
# ---------------------------------------------------------------------------
"$CC" -O2 -shared -static-libgcc -o "$work/inpoutx64.dll" "$here/inpout32_shim.c"
cp "$BIN_DIR/FanForge.exe" "$BIN_DIR/fanforge-cli.exe" "$work/"
ok "inpoutx64.dll built; binaries staged in $work"

export WINEDEBUG=-all
export WINEPREFIX="$work/prefix"
mkdir -p "$WINEPREFIX"
cd "$work"

cli() { wine "$work/fanforge-cli.exe" "$@" 2>/dev/null | tr -d '\r'; }

# ---------------------------------------------------------------------------
step "the command line tool finds the SMC"
# ---------------------------------------------------------------------------
info="$(cli info)"
sed 's/^/    /' <<<"$info"
grep -q "smc         : port-io" <<<"$info" && ok "found the port-io access path" \
    || bad "did not report the port-io path"
grep -q "fans        : 2" <<<"$info" && ok "found both fans" || bad "wrong fan count"
grep -q "control     : FS! mask" <<<"$info" && ok "detected the FS! manual-control style" \
    || bad "did not detect the control style"

# ---------------------------------------------------------------------------
step "fan discovery"
# ---------------------------------------------------------------------------
fans="$(cli fans)"
grep -q "Left side" <<<"$fans" && ok "read the hardware fan label, not a guess" \
    || bad "no fan label"
grep -q "2160 - 5927 RPM" <<<"$fans" && ok "read the fan's own reported speed range" \
    || bad "no speed range"

# ---------------------------------------------------------------------------
step "temperatures"
# ---------------------------------------------------------------------------
temps="$(cli temps)"
sensors=$(grep -c "sp78" <<<"$temps" || true)
[ "$sensors" -eq 8 ] && ok "8 live sensors" || bad "expected 8 sensors, saw $sensors"
if grep -q -- "-127" <<<"$temps"; then
    bad "the dead -127 sensor was shown as a temperature"
else
    ok "the SMC's dead-value sentinel was filtered out"
fi
grep -q "CPU die" <<<"$temps" && ok "sensor names resolved to human ones" \
    || bad "no sensor names resolved"

# ---------------------------------------------------------------------------
step "the write path"
# ---------------------------------------------------------------------------
selftest="$(cli selftest)"
sed 's/^/    /' <<<"$selftest"
grep -q "The write path is confirmed working" <<<"$selftest" \
    && ok "the selftest confirmed the write path end to end" || bad "the selftest failed"

set +e
FAKESMC_SWALLOW_WRITES=1 wine "$work/fanforge-cli.exe" selftest 2>/dev/null > "$work/broken.txt"
broken_status=$?
set -e
if [ "$broken_status" -eq 0 ]; then
    bad "a write the SMC silently dropped was reported as success"
elif grep -q "Self test found problems" "$work/broken.txt"; then
    ok "a swallowed write was caught and reported (exit $broken_status)"
else
    bad "a swallowed write failed without explaining why"
fi

# ---------------------------------------------------------------------------
step "the control loop drives the fans"
# ---------------------------------------------------------------------------
cat > "$work/cfg.ini" <<'EOF'
version=1
poll_interval_ms=500
restore_on_exit=1
emergency_celsius=95
record_csv=1
fan.0.mode=curve
fan.0.sensor=
fan.0.curve=50:2160,75:3600,95:5927
fan.1.mode=curve
fan.1.sensor=
fan.1.curve=45:2000,75:3300,95:5489
EOF

writes_file="$work/writes.log"
rm -f "$writes_file"
FAKESMC_WRITE_LOG="$(winpath "$writes_file")" \
    wine "$work/fanforge-cli.exe" --config "$(winpath "$work/cfg.ini")" watch 8 2>/dev/null \
    | tr -d '\r' > "$work/watch.txt" || true
head -6 "$work/watch.txt" | sed 's/^/    /'

grep -q "all fans returned to system control" "$work/watch.txt" \
    && ok "the loop handed the fans back to the firmware on exit" \
    || bad "did not restore the fans on exit"

writes=0
[ -f "$writes_file" ] && writes=$(grep -c "Tg" "$writes_file" || true)
if [ "${writes:-0}" -gt 0 ]; then
    ok "the loop wrote $writes fan targets to the SMC"
else
    bad "the loop ran but never commanded a fan speed"
fi

csv_dir="$(ls -d "$WINEPREFIX"/drive_c/users/*/AppData/Roaming 2>/dev/null | head -1 || true)"
if [ -n "$csv_dir" ] && [ -f "$csv_dir/FanForge/fanforge-log.csv" ]; then
    ok "the CSV log was written ($(($(wc -l < "$csv_dir/FanForge/fanforge-log.csv") - 1)) rows)"
else
    bad "record_csv was on but no log appeared"
fi

# ---------------------------------------------------------------------------
step "the window"
# ---------------------------------------------------------------------------
if ! command -v Xvfb >/dev/null || ! command -v xwininfo >/dev/null; then
    echo "  SKIP: Xvfb / xwininfo not installed, so the GUI checks cannot run"
else
    # Give the app a configuration that will make it command the fans at once.
    if [ -n "$csv_dir" ]; then
        mkdir -p "$csv_dir/FanForge"
        sed 's/^poll_interval_ms=500/poll_interval_ms=1000/' "$work/cfg.ini" \
            > "$csv_dir/FanForge/config.ini"
    fi

    gui_writes="$work/gui-writes.log"
    rm -f "$gui_writes"
    Xvfb :77 -screen 0 1400x950x24 >/dev/null 2>&1 &
    sleep 2
    DISPLAY=:77 FAKESMC_WRITE_LOG="$(winpath "$gui_writes")" \
        wine "$work/FanForge.exe" > "$work/gui.txt" 2>&1 &
    gui_pid=$!
    sleep 18

    window="$(DISPLAY=:77 xwininfo -root -tree 2>/dev/null | grep '"FanForge"' | head -1 || true)"
    echo "    ${window:-<no window>}"
    geometry="$(grep -oE '[0-9]+x[0-9]+\+' <<<"${window:-}" | head -1 || true)"
    width="${geometry%%x*}"
    if [ -n "$geometry" ]; then
        if [ "${width:-0}" -gt 600 ]; then
            ok "the main window opened (${width}px wide, not the error dialog)"
        else
            bad "a window opened but it is the SMC-missing error dialog (${width}px)"
        fi
    else
        bad "no window appeared at all"
    fi

    if grep -qiE "unhandled exception|page fault" "$work/gui.txt"; then
        bad "the window crashed: $(grep -oiE 'page fault[^)]*' "$work/gui.txt" | head -1)"
    else
        ok "the window ran without crashing"
    fi

    kill "$gui_pid" 2>/dev/null || true
    sleep 1

    gui_count=0
    [ -f "$gui_writes" ] && gui_count=$(grep -c "Tg" "$gui_writes" || true)
    if [ "${gui_count:-0}" -gt 0 ]; then
        ok "the window wrote $gui_count fan targets - the GUI is really driving the SMC"
    else
        bad "the window ran but never wrote to the SMC"
    fi
fi

# ---------------------------------------------------------------------------
printf '\n== %d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
