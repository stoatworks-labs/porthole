# AGENTS.md — bringing an LLM up to speed on Porthole

Orientation for an AI assistant (or a new human) picking this project up cold.
`CLAUDE.md` holds the short command reference; this file explains the model and
the traps.

---

## 1. What this is

A **lens projection warp** for Resolume Arena / Avenue, built on the official
Resolume **FFGL** SDK. C++/GLSL, CMake, public MIT.

The one idea to internalise before changing anything:

> **The Projection control is not a strength knob, it is which lens.** Every
> position on it is a real optic, because the whole slider is one continuous
> family that happens to pass exactly through all five projections anyone has a
> name for.

```
P_k(theta) =  tan(k*theta)/k     k > 0
              theta              k = 0
              sin(k*theta)/k     k < 0
```

| k | projection | radius | what it looks like |
|---|---|---|---|
| +1.0 | Rectilinear | `tan θ` | an ordinary lens — **the identity** |
| +0.5 | Stereographic | `2 tan(θ/2)` | "little planet", angle-preserving |
| 0.0 | Equidistant | `θ` | the classic fisheye |
| −0.5 | Equisolid | `2 sin(θ/2)` | what most real fisheyes actually are |
| −1.0 | Orthographic | `sin θ` | the mirror-ball |

The family is continuous *and* smooth through k = 0 — both one-sided limits are
`theta` — so the slider genuinely sweeps between the named lenses rather than
snapping between five modes.

Three properties fall out of this rather than being arranged, and if you break
one of them you have broken the model:

- **Rectilinear does nothing**, at any field of view. A flat picture
  re-photographed through a flat lens is the same picture.
- **Defish is the exact inverse**, not a similar-looking curve. It is the same
  formula with the roles of source and destination swapped.
- **The frame stays full.** The reference radius is a fixed point: `warpRadius(1)
  == 1` always. Only the interior is redistributed, so strength changes the
  character of the warp instead of shrinking the picture into a black field.

There is deliberately **no wet/dry mix**. Cross-fading two geometries
double-exposes the picture rather than easing between them. The null is Field of
View at zero, where every projection agrees.

## 2. The shape of it

One shader pass. Nothing accumulates between frames and no pixel depends on any
other, so there are no intermediate buffers at all — which is why there is no
`PassBuffer` here the way there is in `old-cathode`.

```
source/Projection.{h,cpp}   the lens family, and the 0..1 -> physical mapping
source/Shaders.{h,cpp}      the GLSL: a mirror of the above, plus geometry,
                            edge modes, chromatic aberration, supersampling
source/Porthole.{h,cpp}     FFGL host glue and the parameter declarations
source/Diag.{h,cpp}         a log file, for the shader that will not compile
tools/phtest/               headless render, probe and round-trip harness
tools/sweep.py              no control is silently dead
tools/verify.sh             all of the above, in one go
```

### The maths exists twice, on purpose

It has to run per-pixel on the GPU, and it has to be readable and testable on
the CPU. Two copies of one formula drift apart — so **`phtest --probe` measures
one against the other** and is the reason the duplication is safe. Change one
copy, change the other, then run the probe.

## 3. Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Add `-DCMAKE_OSX_ARCHITECTURES=arm64` for a much faster dev build.

## 4. Traps

### The macOS one that will get you

**`CMAKE_OSX_ARCHITECTURES` must be set before the first target is created.** Set
it later and CMake silently ignores it — you get an arm64-only binary that the
build log calls a success, and an Intel Resolume that quietly fails to load the
plugin.

**Always verify the artefact, never the log:**

```bash
lipo -archs build/Porthole.bundle/Contents/MacOS/Porthole
```

### A ranged parameter cannot have a ranged default

`SetParamRange` exists, and Resolume honours it. But
`CFFGLPluginManager::SetParamInfo` **clamps an `FF_TYPE_STANDARD` default into
0..1** before returning, and `SetParamRange` can only be called *afterwards*
(it looks the parameter up by ID, so the parameter has to exist first). There is
no `SetParamDefault`. So a parameter declared in degrees cannot declare a
default in degrees — 90 becomes 1.

Hence: **every parameter here is a plain 0..1 float**, and the conversion to
degrees, magnifications and tap counts lives in `Projection.cpp`. Do not "improve"
this by adding ranges without re-checking what the declared default becomes.
(SDK `b1afaf9`, `FFGLPluginManager.cpp`.)

### MaxUV: the texture is bigger than the picture

FFGL hands over a texture that may be larger than the image in it, with `MaxUV`
describing the fraction that was actually drawn. A filter that samples where it
was told never notices. **A warp samples wherever it likes, so it does.**

Two consequences, both handled in `Shaders.cpp` and both easy to undo by accident:

- All geometry happens in **picture space, 0..1**, and `MaxUV` is applied at the
  very last moment, in `fetch()`. The vertex shader deliberately does *not*
  pre-multiply UV by MaxUV the way the SDK's example filters do.
- Every fetch is clamped to at least **half a texel inside** the picture.
  `GL_LINEAR` exactly at the picture edge takes half its weight from padding
  that contains nothing.

### GLSL reserved words

`flat`, `active`, `filter`, `input`, `output`, `sample`, `common`, `partition`,
`resource` and a long tail of others. The failure mode is nasty: the shader
fails to compile at *runtime*, `InitGL` returns `FF_FAIL`, and Resolume shows an
effect that silently does nothing. That is what `source/Diag.cpp` is for.

### The plugin registers itself from a static constructor

`CFFGLPluginInfo` is a file-scope object in `Porthole.cpp` that nothing
references by name. That is why `porthole_core` is an **OBJECT** library and not
a **STATIC** one: in an archive the linker is entitled to drop the whole
translation unit, and you get a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. Do not "tidy" it to STATIC.

```bash
nm -gU build/Porthole.bundle/Contents/MacOS/Porthole | grep _plugMain
```

### Chromatic aberration is independent of distortion, and should stay that way

Lateral chromatic aberration is a change of *magnification* with wavelength. A
lens can have it whether or not its distortion is zero, which is why it is
modelled as the same radial map at three slightly different scales, and why it
still applies at Projection = rectilinear. Tying it to "how much warping is
happening" would make it a drawn-on effect rather than a property of the optic.

## 5. Testing

There is no unit test rig and there cannot usefully be one — the output is a
picture. But a *warp* is unusually testable, because where a pixel went is a
number rather than a matter of taste.

```bash
tools/verify.sh          # everything below, ~3 minutes
```

### `--probe` — the GPU against the C++

Feeds in a picture whose brightness **is** the normalised radius, so the value
that comes back out of an output pixel says, as a number, which source radius
the shader sampled from. That is compared against `Projection.cpp`.

```bash
./build/phtest --probe --set "Projection=0.75" --set "Field of View=0.6"
```

Agreement is typically within 0.6 of an 8-bit level, which is the quantisation
of the ramp rather than the maths.

### `--roundtrip` — does the inverse invert?

Warps a **smooth gradient** one way and back, and measures the error. Smooth on
purpose: what is being measured is the geometry, not the resampler. At moderate
settings this comes back at 0.001 levels.

### Both of them decline to answer, and that is the point

Two things make a region unmeasurable, and **both of them look exactly like a
broken warp if you do not account for them.** Getting this wrong cost two false
alarms while this was being written:

- **Off the frame.** The probe reads along the +x axis, where under a diagonal
  fit the largest radius available is only ~0.87. Defish legitimately wants
  source from beyond the frame edge, so past that the fetch clamps and the
  reading flattens into a straight line of wrong answers.
- **Already destroyed.** Where the fish compressed the picture past 2:1, the
  detail is gone and no correct inverse brings it back.

  The subtle half: the loss is governed by the radius the inverse **reads**
  from, not the one it writes to. Gating on the forward map's gradient at the
  output radius passes the whole frame and then measures the damage anyway.
  Gate on the *inverse's* gradient — `D'(rho) < 0.5` — plus an explicit check
  that the read position is still inside the picture.

When too little survives, `--roundtrip` reports `INCONCLUSIVE` and exits 2
rather than claiming a pass over 0.4% of the frame.

### A dead control is invisible to the compiler

```bash
python3 tools/sweep.py
```

A uniform name that does not match between the C++ and the GLSL is silently
ignored — `glGetUniformLocation` returns -1 and `glUniform` on -1 is a
documented no-op — so a control can be completely dead while everything
compiles, links, loads and renders. Nothing else here catches that.

**The baseline must not be rectilinear.** At Projection = 0 the map is the
identity by construction, so Field of View, Fit, Zoom and Quality all correctly
do nothing and every one of them reads dead. `sweep.py` sits at equidistant for
exactly this reason.

### The harness's own orientation trap

The test-picture builders return **bottom-up** buffers, ready for
`glTexImage2D`. `readBack()` returns **top-down**. Comparing one against the
other index by index silently compares row `y` with row `height-1-y` — which
does not look like a bug, because it produces a large, plausible, and completely
*constant* error that does not change when the warp does. Use `flipRows()`.

## 6. What has never been checked

- **It has never been loaded into Resolume.** Not once. Parameter groups, the
  option dropdowns, Arena's real texture sizes and its premultiplication
  behaviour are all unconfirmed, and those are exactly what the offline harness
  cannot tell you about, because it supplies its own textures. `cmake --install`
  puts the bundle where Arena looks.
- **The Windows build has never been run**, or compiled — only the workflow that
  would do it exists.
- **No performance figure has been taken at all.** The supersampling multiplies
  the per-pixel cost by up to 16 and nobody has measured what that costs at 4K.
- Everything here comes from one M4 Max, never from CI — hosted macOS runners
  have no GPU, so `phtest` cannot run there.

## 6b. Driving Resolume, when you need to

Arena is installed on the development machine and `cmake --install` puts the
bundle where it looks. Notes inherited from `old-cathode`, which did get as far
as loading there:

**Arena is accessible.** Its dialogs answer to `System Events` by button name,
so the update prompt and the composition confirmations can be dismissed
properly. The custom-drawn buttons inside a confirmation are *not* in the tree;
get the window's `position`/`size` and click a computed point, and check which
button you are aiming at first. "New Composition!" offers **New / Cancel / Save
& New** with **Save & New as the highlighted default**, so never dismiss one
with Return.

**Arena has a REST API on `http://127.0.0.1:8080/api/v1`.** `GET .../effects` is
the honest answer to "did the plugin register".

**The trap:** `POST .../clips/{n}/open` **returns 200 and ignores the path you
sent it**, loading an unrelated file instead. Verify what actually loaded by
reading `fileinfo.path` back. There is also **no endpoint for adding an
effect** — `POST .../effects` is 404 — so applying it is a UI job.

**Whatever you do, do not film or modify the operator's own composition.**

## 7. Conventions

- Public repo. "Commit" means commit **and** push.
- Standard AI disclaimer in the README — see the fleet's disclaimer scope.

## Diagnostics

`source/Diag.{h,cpp}` is a small member of the fleet's `diag` family: a log file
only. No crash handler (a plugin has no business installing a process-wide
signal handler inside Resolume) and no bundle command (there is no UI to hang
one off).

It covers the failure that actually happens — `InitGL` returning `FF_FAIL`
because the shader would not compile, which from the operator's side looks like
"the effect does nothing" with no message anywhere. The GL vendor/renderer/
version strings sit next to it because with one shader stage the driver is
nearly always the rest of the answer.
