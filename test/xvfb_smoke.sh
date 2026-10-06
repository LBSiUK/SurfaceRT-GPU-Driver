#!/bin/sh
# xvfb_smoke.sh - run the test clients against a virtual X server.
#
# A quick check that dri3_test, flipdemo and gltri_bench build and run,
# for when the Surface RT is not to hand. Xvfb has no GPU, no DRI3 and no
# page-flipping, so this only proves the clients work end to end:
#   - dri3_test is expected to stop at "DRI3 extension not present"
#   - flipdemo runs in Present copy mode
#   - gltri_bench renders with llvmpipe (software)
#
# Run it inside the grate-build container after `make -C test`:
#   docker run --rm --platform linux/arm/v7 -v "$PWD:/work" \
#       grate-build:armv7 sh test/xvfb_smoke.sh
#
# It installs Xvfb and Mesa's software driver with apk if they are missing.

cd "$(dirname "$0")" || exit 1

for bin in dri3_test flipdemo gltri_bench; do
    [ -x "$bin" ] || { echo "missing ./$bin: run 'make -C test' first" >&2; exit 1; }
done

missing=
for pkg in xvfb xdpyinfo mesa-dri-gallium; do
    apk info -e "$pkg" >/dev/null 2>&1 || missing="$missing $pkg"
done
if [ -n "$missing" ]; then
    SUDO=
    [ "$(id -u)" -ne 0 ] && SUDO=sudo
    # shellcheck disable=SC2086 # word splitting of the package list is intended
    $SUDO apk add -q --no-cache $missing || exit 1
fi

: "${SMOKE_DISPLAY:=:99}"
Xvfb "$SMOKE_DISPLAY" -screen 0 1366x768x24 -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
trap 'kill "$XVFB_PID" 2>/dev/null' EXIT INT TERM
export DISPLAY="$SMOKE_DISPLAY"

i=0
until xdpyinfo >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -gt 60 ] && { echo "Xvfb did not start" >&2; exit 1; }
    sleep 1
done

fail=0

echo "== dri3_test (no DRI3 on Xvfb, exit status 2 expected)"
./dri3_test
rc=$?
[ "$rc" -eq 2 ] || { echo "unexpected exit status $rc"; fail=1; }

echo "== flipdemo 3"
out=$(./flipdemo 3)
rc=$?
printf '%s\n' "$out" | tr '\r' '\n' | grep -v '^$'
[ "$rc" -eq 0 ] || { echo "unexpected exit status $rc"; fail=1; }

echo "== gltri_bench 2000 (llvmpipe)"
LIBGL_ALWAYS_SOFTWARE=1 ./gltri_bench 2000 || fail=1

[ "$fail" -eq 0 ] && echo "smoke test passed" || echo "smoke test FAILED"
exit "$fail"
