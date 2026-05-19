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
| `test/dri3_test.c` | XCB-level functional test exercising DRI3 `pixmap_from_fds` / `fds_from_pixmap` and Present `notify_msc`. |
| `Claude_Context_Surface_RT_Grate.md` | Original mission briefing — kept for context. |

## Where the driver changes live

The opentegra DDX patches are in a fork:

> [`LBSiUK/xf86-video-opentegra`](https://github.com/LBSiUK/xf86-video-opentegra) — branch `dri3-present`

Three logical commits on top of upstream `grate-driver/xf86-video-opentegra`:

1. `exa: export TegraEXAThawPixmap helper for cross-TU use`
2. `Add DRI3 screen support`
3. `Add Present extension support (copy mode)`

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

4. **Build the test client**:
   ```sh
   docker run --rm --platform linux/arm/v7 \
       -v "$PWD/..:/work" grate-build:armv7 \
       bash -c 'cd /work/test && gcc -O2 -Wall -o dri3_test dri3_test.c \
                $(pkg-config --cflags --libs xcb xcb-dri3 xcb-present xcb-sync)'
   ```

5. **Deploy to the device** (Surface RT must be reachable; backs up
   the original .so first):
   ```sh
   scp src/xf86-video-opentegra/src/.libs/opentegra_drv.so \
       leonb@10.101.32.179:/tmp/opentegra_drv.so.new
   scripts/rt_doas.exp \
       "cp /usr/lib/xorg/modules/drivers/opentegra_drv.so \
           /usr/lib/xorg/modules/drivers/opentegra_drv.so.bak; \
        cp /tmp/opentegra_drv.so.new \
           /usr/lib/xorg/modules/drivers/opentegra_drv.so; \
        systemctl restart lightdm"
   ```

6. **Verify** (in an SSH session to the device):
   ```sh
   grep -E "DRI3|Present" /var/log/Xorg.0.log | grep opentegra
   # expected:
   #   (II) opentegra(0): DRI3 initialized
   #   (II) opentegra(0): Present initialized (copy mode)

   DISPLAY=:0 XAUTHORITY=/home/leonb/.Xauthority /tmp/dri3_test
   # expected last lines:
   #   export ok: nfds=1 stride0=4096 ...
   #   import ok: pixmap ... reported 1024x768 depth=32
   #   present complete: kind=1 mode=0 ... msc=<some number>
   #   OK
   ```

## Rollback

```sh
doas cp /usr/lib/xorg/modules/drivers/opentegra_drv.so.bak \
        /usr/lib/xorg/modules/drivers/opentegra_drv.so
doas systemctl restart lightdm
```

## Status / scope

- ✅ DRI3 functionally verified on Tegra30 hardware (dma-buf round-trip
  through the Tegra IOMMU).
- ✅ Present registered; `queue_vblank` returns a real kernel MSC.
- ✅ No 2D EXA regression (XFCE workflow unchanged).
- ⏳ Present page-flip (`check_flip`/`flip`/`unflip`) not implemented;
  server uses Present's copy fallback over DRI3 (correct but
  unaccelerated for vsync).
- ⏳ `glxinfo` still reports `llvmpipe` — Mesa needs an actual
  Tegra30 Gallium driver to use this DRI3 plumbing. That's a
  separate, large work item ("Phase B": port grate-mesa's TGSI
  shader compiler to NIR against modern Mesa).

## Next steps

In rough order of return-on-effort:

1. **Present page-flip** — adds `check_flip`/`flip`/`unflip`. Needs
   CRTC scanout-BO coordination with `drmmode_display.c`.
2. **Package as APK** — write an Alpine `APKBUILD` so the patched
   driver installs cleanly alongside pmOS package management.
3. **Phase 0.5 — grate-mesa rebase** — multi-week. The grate fork is
   on Mesa 19.3 (Jan 2020); the device has Mesa 25.2.7. Includes the
   TGSI→NIR shader compiler port (the briefing's Objective B).
4. **GLES2 conformance survey** — only meaningful after #3.
