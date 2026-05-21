#!/bin/sh
# run_bench.sh - A/B GPU benchmark for the Surface RT.
#
# Runs gltri_bench twice: once on the system driver (llvmpipe, software)
# and once on grate-mesa (the Tegra30 GR3D), and prints both frame rates.
#
# Usage (run as root for the greeter's X auth, or inside a desktop login):
#   doas sh run_bench.sh [triangle-count]
#
: "${DISPLAY:=:0}"
: "${XAUTHORITY:=/var/run/lightdm/root/:0}"
export DISPLAY XAUTHORITY

TRIS="${1:-20000}"
BIN="$(dirname "$0")/gltri_bench"

echo "================ llvmpipe (software rendering) ================"
LIBGL_ALWAYS_SOFTWARE=1 "$BIN" "$TRIS"
echo
echo "================ grate-mesa (Tegra30 GR3D) ===================="
LD_LIBRARY_PATH=/opt/grate-mesa/lib \
  LIBGL_DRIVERS_PATH=/opt/grate-mesa/lib/dri \
  MESA_LOADER_DRIVER_OVERRIDE=tegra \
  "$BIN" "$TRIS"
