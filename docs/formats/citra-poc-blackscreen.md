# Citra proof-of-concept: black-screen investigation (session 10) — RESOLVED session 11

## Resolution (session 11)

**Root cause was the specific old Citra build, not this project's code, not the emulator
class in general.** The `citra-windows-msys2-20240303-0ff3440` nightly (SDL2 frontend) used
in sessions 1-10 was one of the last nightlies before Citra's March 2024 shutdown. Switching
to **Azahar** (`azahar-emu/azahar`, the actively-maintained successor merged from Lime3DS +
PabloMK7's fork — see `HANDOFF.md` session 11 for the research writeup) immediately got past
the black screen: Azahar's release notes document a fixed issue ("certain older homebrew
software would get stuck on a black screen at boot"), which matches this symptom exactly.

With Azahar, real geometry rendered on the first try, which then exposed **three genuine
bugs in this project's own code** that the old black-screen bug had been masking:
1. `loadScene()`'s index-expansion loop ignored each mesh's `vertexOffset` (indices in
   `.nfsm` are mesh-local, not global into the shared vertex pool) — fixed in `main.c`.
2. The track was drawn with the car's diffuse texture bound (wrong UVs → visual noise) —
   fixed by using a flat placeholder color for the track until a real track texture exists.
3. The car mesh's corrective Z-up→Y-up rotation and its display spin were composed in the
   wrong order, so the spin visibly tumbled front-over-back instead of turning like a
   turntable — fixed by re-ordering the `Mtx_Rotate*` calls.

All three were confirmed via user screenshots of the running Azahar window — first a
correctly-lit, correctly-textured but garbled/mis-rotated car, then a clean, upright,
`NISSAN`-badge-legible, correctly-spinning 240SX. **Task #34 is complete.** See `HANDOFF.md`
session 11 for full details. The investigation below is kept for reference (it correctly
ruled out this project's rendering code as the black-screen cause, which was the right call
at the time) but should not be re-investigated further.

## Original status (session 10, superseded above)

Task #34 (render car mesh + track segment in Citra, visually verify) is **blocked on an
apparent Citra/environment display issue, not a bug in this project's code**. Everything
this project controls has been verified correct via an `sdmc:/nfsu2_debug.txt` runtime log
(see below); the emulator window nonetheless shows solid black.

## What's confirmed working (do not re-investigate these)

1. **The original host-process segfault is fixed.** Citra Nightly 2104 (`HEAD-0ff3440`)
   crashes the host process on `C3D_DrawElements` regardless of index/vertex data --
   confirmed via bisection down to a hardcoded 3-vertex/3-index triangle. Fix: expand all
   indexed geometry into flat unindexed vertex arrays at load time (`loadScene` in
   `source/main.c`) and draw with `C3D_DrawArrays` instead. Do not reintroduce
   `C3D_DrawElements` in this Citra build.
2. **The depth-test convention was wrong and has been fixed.** PICA200/citro3d uses a
   reversed depth convention: devkitPro's own depth-tested examples
   (`graphics/gpu/composite_scene`, `graphics/gpu/particles`) clear depth to `0` and test
   with `GPU_GREATER`, not `GPU_LEQUAL` with a conventional far=1 clear. `main.c` now uses
   `C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL)` to match. (This was a real bug, worth
   keeping fixed, but did not by itself resolve the black screen.)
3. **The full real scene runs cleanly end-to-end.** Per `nfsu2_debug.txt`: gfx/romfs init,
   `C3D_Init`, render target creation, shader bind, texture import (605-mesh car + 482-mesh
   track, real HUFF/DXT-decoded texture) all succeed, and the main loop completes multiple
   full frames (`frame N begin` -> track draw done -> car draw done -> `C3D_FrameEnd` -> frame
   end done) with zero crashes, across many seconds of runtime.
4. **A hardcoded fullscreen-ish triangle with an identity MVP** (bypassing all mesh data,
   camera matrices, and texture sampling -- flat constant red color) *also* fails to show
   up, while still logging "fullscreen tri drawn" every frame with no crash. This rules out
   camera/projection math, mesh data, and texture content as the cause.
5. **The stock, unmodified devkitPro example `graphics/gpu/simple_tri` also renders solid
   black** in this same Citra binary/environment. This is the decisive test: it rules out
   this project's code entirely. Whatever is wrong is in Citra's display/window pipeline or
   this environment, not in `source/main.c`, the shaders, or the asset pipeline.
6. **Not a screenshot-capture artifact.** Two different capture methods were tried against
   the live Citra window: GDI `BitBlt`-equivalent (`Graphics.CopyFromScreen`) and
   `PrintWindow` with `PW_RENDERFULLCONTENT` (which correctly captures DirectX/OpenGL
   swap-chain/DWM-composited windows that plain `BitBlt` often shows as black for). Both
   show the same solid black client area for `simple_tri`. The user also independently
   confirmed "All I see is a black screen" watching Citra live, not just via captured
   screenshots.
7. Citra's log (`%APPDATA%/Citra/log/citra_log.txt`) shows a normal startup for these runs:
   valid OpenGL context (`GL_VERSION: 4.3.0 NVIDIA 596.49`, real RTX 4060 GPU, not a
   software/headless fallback), no GL context creation errors, no fatal errors after ROM
   load. The only anomalies are benign (`Service.FS` warning about 3dsx files not having a
   program id, `Service.HTTP` missing ClCertA -- both normal/expected for homebrew `.3dsx`
   loads via Citra's SDL2 frontend, unrelated to rendering).

## Leading hypothesis (not yet confirmed)

Given a real, valid OpenGL 4.3 NVIDIA context is created but the window never visibly
presents content -- for both our code and a known-good stock example -- the most likely
remaining explanation is an environment-level windowing/compositing issue specific to this
dev machine/session (e.g. a remote-desktop/streamed session interfering with how the SDL2
window's OpenGL swap chain gets composited/presented to whatever is capturing or relaying
the screen), rather than anything fixable from within the NFSU2-3DS project or Citra's
config. This has not been proven; it's the leading theory after ruling out (1)-(6) above.

## Recommended next steps (in order)

1. **Try Citra's own screenshot feature from inside the emulator** (not host-side capture):
   Citra has a hotkey (default not yet checked) or a File menu-driven capture that grabs
   the emulated framebuffer directly from GPU memory rather than compositing the OS window.
   If *that* also comes back black, it proves the emulated framebuffer itself is empty
   (pointing back at our code or a deeper Citra bug after all). If it shows real content,
   it confirms the OS-level window presentation is the broken link, and the underlying
   render path is actually fine.
2. **Try the Qt frontend (`citra-qt.exe`) instead of the SDL2 `citra.exe`** if available in
   the same download -- different window/GL-context management, useful to isolate whether
   this is SDL2-window-specific.
3. **Test on real hardware** (New 3DS XL via `.cia` + FBI, or `.3dsx` via Homebrew
   Launcher), as the user offered. This sidesteps the whole emulator-display question
   entirely and is the most authoritative test available. Given (1)-(6) above, there is a
   reasonable chance the actual render output is already correct and this is purely an
   emulator/environment display problem.
4. If real hardware also shows nothing, *then* re-open investigation of the render path
   itself (texture format/subtexture setup, attribute permutation encoding, etc.) --
   but that would contradict finding #5 above (stock example also black) unless hardware
   and Citra diverge for some other reason.

## How to re-run the diagnostic build

- `source/main.c` has two inert, `#ifdef`-gated debug hooks left in place for future
  sessions: `BISECT_FLAT_COLOR` (bypass texture, flat red constant) and
  `BISECT_FULLSCREEN_TRI` (draw one hardcoded triangle with identity MVP, in the main loop
  right after `C3D_FrameDrawOn(targetTop)`). Both are OFF by default; `#define` them above
  their `#ifdef` in `main()` and rebuild to re-enable.
- `dlog()` writes to `sdmc:/nfsu2_debug.txt`, readable on the host at
  `%APPDATA%/Citra/sdmc/nfsu2_debug.txt` after a run -- the fastest way to confirm the
  program is executing correctly without relying on visual output at all.
- Standard rebuild: `subst Z: "<repo>\3DS Files"` then
  `C:\devkitPro\msys2\usr\bin\bash.exe -lc "cd /z && DEVKITARM=/opt/devkitpro/devkitARM DEVKITPRO=/opt/devkitpro make"`,
  then `subst Z: /D`.
