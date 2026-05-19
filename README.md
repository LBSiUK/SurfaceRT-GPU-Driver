# SurfaceRT-GPU-Driver

Userspace graphics work for the Microsoft Surface RT (2012) running
postmarketOS on the Tegra 3 (T30) SoC. Cross-build tooling, an XCB
test client, and project notes — the actual driver patches live in
their own forks.

## What's here

| Path | Purpose |
|---|---|
| `Dockerfile.grate-build` | Alpine 3.23 / armv7 container matching the device's libc/Xorg ABI. Built via `docker buildx build --platform linux/arm/v7 -t grate-build:armv7 .` |
| `scripts/rt_doas.exp` | Expect wrapper that drives `ssh + doas` password prompts for headless deploys. |
| `test/dri3_test.c` | XCB-level functional test exercising DRI3 `pixmap_from_fds` / `fds_from_pixmap`, Present `notify_msc`, and page-flip vs copy. |
| `test/flipdemo.c` | Fullscreen visual demo — continuously page-flips a sweeping bar, with a live flip/copy + fps readout. |
| `packaging/xf86-video-opentegra/APKBUILD` | Alpine `APKBUILD` for the patched driver; `abuild`-able inside the container. |
| `Claude_Context_Surface_RT_Grate.md` | Original mission briefing — kept for context. |

## Where the driver changes live

The opentegra DDX patches are in a fork:

> [`LBSiUK/xf86-video-opentegra`](https://github.com/LBSiUK/xf86-video-opentegra) — branch `dri3-present`

Four logical commits on top of upstream `grate-driver/xf86-video-opentegra`:

1. `exa: export TegraEXAThawPixmap helper for cross-TU use`
2. `Add DRI3 screen support`
3. `Add Present extension support (copy mode)`
4. `Add Present page-flip (check_flip/flip/unflip)`

## Reproducing

1. **Build the cross-compile image** (one-time, ~3 min):
   ```sh
   docker buildx build --platform linux/arm/v7 -t grate-build:armv7 \
       -f Dockerfile.grate-build .
   ```

2. **Clone the opentegra fork** with the dri3-present branch:
   ```sh
   mkdir -p src && cd src
   git clone -b dri3-present \
       https://github.com/LBSiUK/xf86-video-opentegra.git
   ```

3. **Build the driver** (armv7 .so, ~268 KB):
   ```sh
   docker run --rm --platform linux/arm/v7 \
       -v "$PWD/xf86-video-opentegra:/work" grate-build:armv7 \
       bash -c 'cd /work && export LDFLAGS="-Wl,-z,lazy" \
                CFLAGS="-O2 -std=gnu99" && \
                ./autogen.sh --prefix=/usr && make -j4'
   ```

   Output: `src/xf86-video-opentegra/src/.libs/opentegra_drv.so`

4. **Build the test clients**:
   ```sh
   docker run --rm --platform linux/arm/v7 \
       -v "$PWD/..:/work" grate-build:armv7 bash -c '
       cd /work/test
       gcc -O2 -Wall -o dri3_test dri3_test.c \
           $(pkg-config --cflags --libs xcb xcb-dri3 xcb-present xcb-sync)
       gcc -O2 -Wall -o flipdemo flipdemo.c \
           $(pkg-config --cflags --libs xcb xcb-present)'
   ```

5. **Build the APK** (packages the driver for a clean `apk` install):
   ```sh
   docker run --rm --platform linux/arm/v7 -v "$PWD/..:/work" \
       grate-build:armv7 bash -c '
       abuild-keygen -a -i -n
       cd /work/packaging/xf86-video-opentegra
       export REPODEST=/work/packaging/apk-out
       abuild checksum && abuild -r'
   ```
   Output: `packaging/apk-out/packaging/armv7/xf86-video-opentegra-*.apk`

6. **Deploy to the device** (Surface RT must be reachable):
   ```sh
   scp packaging/apk-out/packaging/armv7/xf86-video-opentegra-*.apk \
       leonb@10.101.32.179:/tmp/
   # apk 3.x prompts interactively; </dev/null makes it proceed
   scripts/rt_doas.exp \
       "apk add --allow-untrusted /tmp/xf86-video-opentegra-*.apk </dev/null"
   scripts/rt_doas.exp "systemctl restart lightdm"
   ```
   This upgrades over the stock community package; the original .so
   also stays backed up at `opentegra_drv.so.bak`.

7. **Verify** (in an SSH session to the device):
   ```sh
   grep -E "DRI3|Present" /var/log/Xorg.0.log | grep opentegra
   # expected:
   #   (II) opentegra(0): DRI3 initialized
   #   (II) opentegra(0): Present initialized (page-flip)

   DISPLAY=:0 XAUTHORITY=/var/run/lightdm/root/:0 /tmp/dri3_test
   # expected last lines:
   #   present N: mode=1 (FLIP) ...
   #   FLIP CONFIRMED: page-flip path engaged
   #   OK

   # flipdemo: a watchable fullscreen demo — sweeping bar, live fps
   DISPLAY=:0 XAUTHORITY=/var/run/lightdm/root/:0 /tmp/flipdemo 15

   # Run both against the lightdm greeter — a running desktop
   # compositor legitimately forces Present back to copy mode.
   ```

## Rollback

```sh
# revert to the stock community package
doas apk add xf86-video-opentegra
doas systemctl restart lightdm
# or, if the community repo is unreachable, restore the backup .so:
#   doas cp /usr/lib/xorg/modules/drivers/opentegra_drv.so.bak \
#           /usr/lib/xorg/modules/drivers/opentegra_drv.so
```

## Status / scope

- ✅ DRI3 functionally verified on Tegra30 hardware (dma-buf round-trip
  through the Tegra IOMMU).
- ✅ Present registered; `queue_vblank` returns a real kernel MSC.
- ✅ No 2D EXA regression (XFCE workflow unchanged).
- ✅ Present page-flip (`check_flip`/`flip`/`unflip`) implemented and
  verified on hardware — a fullscreen present becomes a zero-copy
  `drmModePageFlip` scanout swap; non-flippable requests fall back to
  Present's copy path.
- ⏳ `glxinfo` still reports `llvmpipe` — Mesa needs an actual
  Tegra30 Gallium driver to use this DRI3 plumbing. That's a
  separate, large work item ("Phase 2": build grate-mesa and port its
  TGSI shader compiler to NIR against modern Mesa).

## Next steps

In rough order of return-on-effort:

1. **Phase 2 — grate-mesa build** — cross-build the grate Gallium
   driver (branch `22.0.1`, the newest grate-maintained Mesa) and
   smoke-test `grate_dri.so` on hardware. The path off llvmpipe.
2. **TGSI→NIR shader compiler port** — the grate fragment/vertex
   compilers are TGSI-only; modern Mesa (25.x) consumes NIR.
3. **GLES2 conformance survey** — only meaningful after #1/#2.

Phase 2 is staged session-by-session in
[`docs/PHASE2_ROADMAP.md`](docs/PHASE2_ROADMAP.md).
