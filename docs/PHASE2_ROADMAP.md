# Phase 2 Roadmap — Hardware 3D on the Tegra 3

**Goal:** get the Tegra 3 (T30) GR3D engine actually executing OpenGL ES,
so `glxinfo`/`es2_info` report a hardware renderer instead of `llvmpipe`,
and a Wayland compositor (Phosh/`phoc`) can run GLES2-accelerated.

**Where Phase 1 left it:** `xf86-video-opentegra` now exposes DRI3 +
Present (incl. page-flip) — the *display path* is done and verified. What
is missing is a userspace **Mesa Gallium driver** for the GR3D. That is
all of Phase 2.

This document stages Phase 2 into sessions. Each "session" is roughly one
full-budget working block. Read it alongside `HANDOFF.md` (live state)
and `joyful-bubbling-moonbeam.md` (the original exploration notes).

---

## Strategic framing — two paths to hardware 3D

The grate Gallium driver lives in the `grate-driver/mesa` fork. Its newest
maintained branch is **`22.2.4`** (Mesa 22.2, Dec 2022 — discovered after
this doc was first written; supersedes the earlier `22.0.1` choice, adds
a Mesa 22.2 rebase + 7 extra grate commits). The device runs stock
**Mesa 25.2.7**. There are two ways to bridge that gap:

- **Path A — ship grate-mesa 22 wholesale.** Build *all* of grate-mesa
  22.0.1 (`libGL`/`libEGL`/`libgbm`/`grate_dri.so`/`swrast`) and install
  it as a parallel or replacement GL stack. The grate driver stays
  TGSI-based and runs in its native Mesa 22 environment — **no rebase, no
  shader-compiler port needed for first light.** `libGL.so.1` is ABI
  stable, so apps linked against Mesa 25 still run against Mesa 22's
  libGL.
- **Path B — rebase grate onto Mesa 25.** Port the driver forward
  22 → 25, which forces the TGSI→NIR shader-compiler rewrite (Mesa
  dropped TGSI from Gallium drivers in the 24→25 dril refactor). This is
  the multi-week piece.

**Recommended order: Path A first, Path B as a follow-on.** Path A is the
fastest route to *any* hardware 3D and de-risks everything — if grate
can't drive the GR3D on the current kernel at all, that is found in days,
not weeks. Path B (NIR port + rebase) only becomes worthwhile *after*
grate is proven to render on this hardware, and is also what is needed
for long-term integration with the device's modern Mesa.

---

## Build infrastructure — fix this first

The opentegra builds used a **qemu-emulated armv7 container**. That is
fine for a 270 KB DDX; it is painful for Mesa. Apple Silicon cannot run
AArch32 natively, so emulation is unavoidable *if building "natively"
inside an armv7 container* — every Mesa `ninja` run would take tens of
minutes.

**Set up a real cross-compile instead.** Build armv7 Mesa with an
aarch64-hosted cross toolchain (the compiler runs at native speed, only
the *output* is armv7) driven by a meson cross file. This is a 5–20×
speed-up on the iterative build work and is worth the Session 1 setup
cost. Keep `ccache` (the `.ccache/` dir already exists).

Container base: **Alpine 3.17** (Python 3.10, meson 0.63, mako 1.2 —
contemporaneous with Mesa 22; Alpine 3.23's meson 1.x risks deprecation
breakage). Do NOT reuse `grate-build:armv7` (Alpine 3.23).

---

## Session 1 — Build grate-mesa 22.2.4

**Goal:** a clean `grate_dri.so` + `libGL`/`libEGL`/`libgbm` from the
`22.2.4` branch. Prove the toolchain; do not touch the device yet.

**Steps**
1. ✅ Repoint the worktree (done 2026-05-20):
   `src/grate-mesa-grate/` is on `grate-22.2.4` (tip `4621b88ee2e`).
2. Write `Dockerfile.grate-mesa-build` (Alpine 3.17) + a meson cross file
   for armv7. Tag the image `grate-mesa-build`.
3. `meson setup build -Dgallium-drivers=grate,swrast -Dvulkan-drivers=
   -Dplatforms=x11 -Dgles2=true -Dglx=dri -Degl=enabled -Dgbm=enabled
   --cross-file armv7.txt`. Resolve missing-dependency errors by editing
   the Dockerfile package list.
4. `ninja -C build`. Expect a list of compile errors — document each in a
   session log. Likely: libdrm version vs grate's 2022 expectations,
   kernel UAPI header drift, meson quirks.

**Deliverable:** `build/src/gallium/targets/dri/grate_dri.so` plus the
Mesa 22 GL libraries. Toolchain proven.

**Risks:** the grate driver on 22.0.1 vendors its own Tegra DRM under
`src/gallium/drivers/grate/drm/` (no external libdrm_tegra needed) — good
— but that vendored code is from 2022 and may not match the device's
kernel UAPI; that surfaces here or in Session 2.

---

## Session 2 — First GL app against grate ✅ done (2026-05-20)

**Goal:** run a minimal GLES2 app through the grate driver on the actual
GR3D and capture what happens.

**Result:** "off llvmpipe" milestone reached. `glxinfo` and `es2_info`
both report `Vendor: Grate / Renderer: Tegra / OpenGL ES 2.0 Mesa
22.2.4` via `tegra_dri.so` (no llvmpipe). `es2tri` built a real GR3D
cmdstream and submitted it via `DRM_TEGRA_SUBMIT`; kernel logged
`tegra_drm_copy_and_patch_cmdstream: invalid class id 0x0`. Three
diagnoses captured for Session 3 (see HANDOFF "Session 2"):
(1) cmdstream UAPI mismatch — the blocker;
(2) 11-bit INDEX_COUNT assertion blocking large draws;
(3) cap-reporting bug crashing on context teardown.

**Steps**
1. Install the Session 1 build into a prefix on the device, e.g.
   `/opt/grate-mesa` (do NOT overwrite system Mesa). Package it as an APK
   later; for now `scp` a tarball.
2. Run `es2gears` (or a hand-written one-triangle GLES2 program) with
   `LD_LIBRARY_PATH=/opt/grate-mesa/lib
   LIBGL_DRIVERS_PATH=/opt/grate-mesa/lib/dri
   MESA_LOADER_DRIVER_OVERRIDE=tegra LIBGL_DEBUG=verbose`. (The grate
   driver registers under the filename `tegra_dri.so`, not
   `grate_dri.so` — `src/gallium/targets/dri/meson.build` maps both
   `with_gallium_tegra` and `with_gallium_grate` to `tegra_dri.so`.)
3. This is the first time Phase 1's DRI3/Present plumbing carries buffers
   from a *real GPU driver*. Watch `dmesg` for Tegra30 `host1x` job
   ioctls — even a crash that reaches real ioctls is progress.

**Deliverable:** either a hardware-rendered frame, or a precise diagnosis
of where the grate submit path diverges from the running kernel.

**Risks:** host1x command-submission UAPI mismatch between grate-mesa's
2022 vendored DRM and the `linux-postmarketos-grate` 6.16 kernel is the
single most likely blocker. If it bites, Session 3 absorbs it.

---

## Session 3 — Stabilise grate on kernel 6.16

**Goal:** reproducible hardware 3D for simple shaders.

**Steps**
1. Fix whatever Session 2 surfaced — align grate-mesa's
   `drivers/grate/drm/uapi_v1/` job/pushbuf code with the kernel's
   `tegra_drm.h` UAPI. Cross-check the kernel source of
   `linux-postmarketos-grate`.
2. Use the `grate` RE harness (`src/grate/` — cmdstream decoder) to
   compare the command streams Mesa emits against known-good captures.
3. Get an `es2gears`-tier frame rendering stably.

**Deliverable:** a simple GLES2 program rendering on the GR3D, repeatably.
This is the "off llvmpipe" milestone.

---

## Session 4+ — GLES2 coverage

**Goal:** widen from "a triangle" toward usable GLES2.

**Steps**
1. Run `glmark2-es2` and `piglit` (`tests/gles2.py`); triage failures
   into: hardware-incapable (→ permanent `skip` in the grate caps),
   missing CSO/state in `grate_state.c`, missing `pipe_context` entry
   points.
2. Implement state/entry-points opcode-by-opcode, driven by failing
   tests rather than speculatively.
3. Track coverage in a `CONFORMANCE.md` in the grate driver dir.

**Deliverable:** a documented GLES2 coverage level; ideally enough for a
Wayland compositor (`phoc`/`cage` with `WLR_RENDERER=gles2`).

**Reality check:** the grate driver was always experimental and was never
GLES2-complete. Some features will be permanent `skip`s. "Partial
hardware GLES2" is a realistic and worthwhile Phase 2 outcome.

---

## Long tail — Path B: rebase 22 → 25 + TGSI→NIR

Only start this once grate renders on Mesa 22 (Session 3 done). Needed for
long-term integration with the device's stock Mesa and for modern
features.

- **Shader compiler:** rewrite `grate/fp/tgsi.c` and `grate/vp/tgsi.c` as
  NIR consumers feeding the existing IR-agnostic VLIW packers
  (`grate/fp/pack.c`, `grate/vp/pack.c`). Model the NIR backend on an
  in-tree small-GPU driver — **`etnaviv`** is the closest analog (another
  embedded tiler with its own ISA, fully NIR-based); `lima` and
  freedreno `ir3` are secondary references.
- **Rebase staging:** 22 → 23 → 24 → 25, smallest hops. The 24→25 step is
  the dril common-loader refactor and TGSI removal — the NIR port must
  land before/with it.
- `grate_screen.c` advertises `PIPE_SHADER_IR_NIR`; add
  `nir_shader_compiler_options`.

This is the multi-week part. Stage it across several sessions, each
ending on a branch that still builds.

---

## Integration & packaging

Once grate renders: decide whether to ship grate-mesa as the **system**
GL (replacing 25.2.7) or keep it **opt-in** behind env vars. Either way,
package it as an Alpine APK the same way `xf86-video-opentegra` was
(`packaging/`), for clean install/rollback via `apk`.

---

## Risks & known unknowns

| Risk | Mitigation |
|---|---|
| host1x submit UAPI: grate-mesa 2022 vs kernel 6.16 | Surfaces Session 2; Session 3 aligns the vendored DRM with the kernel's `tegra_drm.h`. |
| grate driver is incomplete by design | Accept "partial GLES2"; mark hardware gaps as `skip`. |
| Mesa 22 `libGL` coexisting with the pmOS Mesa-25 userland | `libGL.so.1` ABI is stable; low risk — but test Xorg GLX + a few apps. |
| qemu build times | Switch to a real armv7 cross-compile (see Build infrastructure). |
| GR3D opcode coverage (e.g. `dFdx/dFdy`) | Some GLES2 features may simply not exist on T30 → `skip`, not `fix`. |

## Fallback

If grate cannot drive the GR3D on this kernel at all (Session 2/3 dead
end), the hardware-3D goal may need kernel-side work beyond Phase 2's
scope, or grate may be too incomplete to be useful. In that case
`llvmpipe` stays — but Phase 1's DRI3 + Present page-flip work stands on
its own as a real improvement to the 2D/display path.
