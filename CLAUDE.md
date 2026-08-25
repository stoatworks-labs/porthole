# porthole

Lens projection warp (fisheye and defish) as an FFGL effect for Resolume
Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows
`.dll`. Public MIT repo.

Read `AGENTS.md` before changing the projection maths.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/phtest --out /tmp/frame.png`
- List parameters: `./build/phtest --list`
- Put real footage through the real shader (for the project video):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/phtest --pipe --width W --height H [--script cues.txt] | ffmpeg …`

## OpenFX build
- `source/ofx/PortholeOFX.cpp` → `build/Porthole.ofx.bundle` (target `PortholeOFX`,
  `-DBUILD_OFX=OFF` to skip) for Resolve/Nuke/Natron/Vegas. It links
  `Projection.cpp` directly — the lens maths still has one home — but mirrors the
  fragment shader's pixel machinery (edge modes, supersample grid, chromatic
  triple) on the CPU. Change the shader's pixel machinery, change this too.
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Smoke test: `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.porthole`
- Identity proof: add `--set projection=0 --set chromatic=0 --set quality=0` → 0 bytes differ.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Final Cut Pro / Motion build (fxplug/)
- Apple's FxPlug 4. **The pattern doc is `resolume-luma-keyer/docs/FXPLUG-PORT.md`** —
  read it before changing anything structural; it holds the fleet UUID registry
  and the trap list.
- Needs Apple's SDK at `/Library/Developer/SDKs/FxPlug.sdk` (login-gated, **not
  redistributable — CI cannot build this**). Off by default:
  `cmake -B build-fxplug -DBUILD_FXPLUG=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build-fxplug`
- Sign (**mandatory** — an unsigned FxPlug plugin does not load):
  `./fxplug/sign.sh "build-fxplug/Stoatworks Porthole.app"`
- Install: copy the .app to /Applications **and launch it once**. Copying alone
  does not register the service, and neither reliably does `pluginkit -a`.
- Host-free render test: `cmake --build build-fxplug --target porthole-tiletest &&
  ./build-fxplug/fxplug/porthole-tiletest`
- The warp reads from anywhere in the source, so unlike a per-pixel effect this
  declares `kFxPropertyKey_NeedsFullBuffer` and `-sourceTileRect:` returns the
  WHOLE source image. Output pixels are placed by their position in the full
  image, not in the tile — `testTiledMatchesWhole` is what guards that.
- The maths is NOT duplicated here: `Projection.cpp` is compiled straight in,
  the same way the OpenFX build does it. `porthole_core` is deliberately not
  used — it is an OBJECT library carrying the FFGL SDK and OpenGL with it.

## Verify
- Everything: `tools/verify.sh`
- GLSL vs C++ maths: `./build/phtest --probe`
- Defish really inverts: `./build/phtest --roundtrip`
- No dead controls: `python3 tools/sweep.py`
- **Before a release**, the shipped Windows artefact in a real Arena:
  `../plugin-bench/arena/gate.sh porthole` — loads the actual `.dll` on the win-lab
  VM and checks registration, the control surface the host sees, FFGL's
  16-char name truncation, and that every control still moves the picture.
  Takes minutes, needs the VM, and is deliberately NOT in `tools/verify.sh`.
  Its expectation lives at `plugin-bench/arena/expect/porthole.json`, so a
  deliberate change to the controls changes that file too.

## Notes
- One shader pass, no intermediate buffers. The effect is the GLSL; the C++ is
  host glue plus the lens family.
- The Projection slider is *which lens*, not how much — every position is a real
  projection. Rectilinear (Projection = 0) is the identity by construction.
- The lens maths exists twice, in `Projection.cpp` and in `Shaders.cpp`.
  `--probe` measures one against the other. Change one, change both, run it.
- All host parameters are 0..1 and mapped internally. `SetParamInfo` clamps a
  standard default into 0..1 before `SetParamRange` can widen it, so a degrees
  parameter cannot declare a degrees default.
- Geometry is in picture space; `MaxUV` is applied only at the fetch, and every
  fetch stays half a texel inside the picture.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- `flat` and `active` are GLSL reserved words. Shader errors only surface at
  runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. It logs the GL vendor/renderer/version
next to it.
