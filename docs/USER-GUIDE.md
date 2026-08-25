# Porthole user guide

Porthole is **a variable fisheye and defish lens warp for [Resolume](https://resolume.com) Arena
and Avenue**, as an FFGL effect.

It does not draw a bulge. It **re-photographs the picture through a different lens**: every
output pixel stands for a ray at some angle off the optical axis, the lens model says where a ray
at that angle lands on the image, and the picture is resampled accordingly.

> **Before you rely on this:** the projection maths is verified numerically — the GLSL is measured
> against an independent C++ implementation of the same maths across 120 combinations of fit,
> projection, field of view and direction, agreeing to 0.6 of an 8-bit level (which is the
> quantisation of the test ramp, not the maths), and the fish/defish round trip lands within
> 0.001 of a level at moderate settings. It **has since been run inside Resolume on real
> content**.
>
> Still open: **performance has never been measured**, and while Windows binaries are built and
> shipped in the release, the plugin has only been exercised in Resolume on macOS.
>
> This codebase was created with AI assistance, directed and reviewed by a human author.

---

## Installing

Drop the plugin bundle into Resolume's FFGL folder and restart Resolume:

```
macOS    ~/Documents/Resolume Arena/Extra Effects/
         (or /Users/Shared/Resolume Arena/Extra Effects/)
Windows  %USERPROFILE%\Documents\Resolume Arena\Extra Effects\
```

Avenue uses the same layout under its own folder name. Porthole then appears in the effects
browser.

**Needs Resolume Arena or Avenue 7.3.1 or newer.**

The macOS builds are **Developer ID-signed and notarised**, so the bundle simply loads — there is
nothing to clear and no `xattr` step. The Windows builds are not code-signed, but plugin files are
not gated the way `.exe` files are, so Resolume loads them normally; only the installer trips
SmartScreen, once: **More info** → **Run anyway**.

### OpenFX hosts (Resolve, Vegas, Nuke, Natron)

Porthole also ships as an OpenFX plugin — same effect, same controls. Copy
`Porthole.ofx.bundle` from the `-ofx-` download into the OpenFX folder and
restart the host:

```
macOS    /Library/OFX/Plugins/
Windows  C:\Program Files\Common Files\OFX\Plugins\
Linux    /usr/OFX/Plugins/
```

---

## Projection is *which lens*, not how much

This is the control that matters, and the thing to understand about it is that it is **not a
strength knob**. It is a single continuous family that passes exactly through all five
projections that have names:

| Projection | Formula | What it is |
|---|---|---|
| **Rectilinear** | `r = tan θ` | An ordinary lens — and the identity |
| **Stereographic** | `r = 2 tan(θ/2)` | "Little planet"; preserves angles |
| **Equidistant** | `r = θ` | The classic fisheye |
| **Orthographic** | `r = sin θ` | The mirror-ball |

![The repo's geometry test card through the default equidistant lens: the grid shows the shape of the distortion, the rings confirm the map stays radial, and the frame stays full.](hero.jpg)

Three behaviours follow from modelling it this way rather than fitting a curve that looks about
right. All three are **consequences, not features** — which is why they are exact:

- **Rectilinear does nothing at all**, at any field of view. A flat picture re-photographed
  through a flat lens is the same picture; the maths says so without being told. If you park the
  Projection control there and see any change, something is wrong.
- **Defish exactly undoes fish.** It is the same formula with source and destination swapped, not
  a similar-looking inverse curve.
- **The frame always stays full.** The reference radius is a fixed point of the map, so only the
  interior is redistributed. Strength changes the *character* of the warp instead of shrinking
  the picture into a black field.

![The same card, defished: pincushion instead of barrel, with honest transparent corners.](defish.jpg)

*Undoing a fisheye genuinely wants picture from beyond the frame, and there is none — so the
corners go transparent rather than being invented.*

---

## Field of View is the null

There is deliberately **no wet/dry mix**. Cross-fading two different geometries double-exposes
the picture rather than easing between them — you would see both warps at once, not a blend.

**The null is Field of View at zero**, where every projection agrees anyway. That is the control
to animate if you want the effect to come and go.

---

## Audio reactivity

Resolume can drive any of these sliders from its own audio analysis: click the dropdown beside
a parameter, choose **FFT**, and pick a band and gain. No plugin setting is involved.

Porthole's controls happen to be the classic pump targets:

- **Field of View** on the low band is the whole "lens breathes with the kick" move — and
  because zero is the null, the picture *relaxes to undistorted* between hits rather than
  sitting in some second geometry.
- **Chromatic** on hits gives the fringing snap that usually takes a separate effect.
- **Zoom** on overall level, kept subtle, adds the push a camera operator would.

Leave **Projection** alone: it is a dropdown, and audio-driving it cuts between lenses rather
than warping one.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| **Plugin doesn't appear in Resolume** | Wrong folder, or Resolume older than 7.3.1. |
| **macOS: installed but never loads** | The current builds are signed and notarised, but an older download may still be quarantined — and Resolume skips a quarantined plugin silently. `xattr -dr com.apple.quarantine <bundle>`. |
| **Rectilinear is changing the picture** | It shouldn't, at any field of view. That is a bug worth reporting. |
| **Corners go transparent when defishing** | Correct and deliberate — that picture does not exist. Composite something behind it. |
| **I want to fade the effect in** | Animate Field of View to zero rather than looking for a mix control. |
| **Defish doesn't perfectly undo my fisheye footage** | It exactly inverts *Porthole's* fish. Real camera glass is not one of these five ideal projections. |
| **Performance is poor** | Genuinely unmeasured — please report it with your raster and GPU. |

---

## See also

- [README](../README.md) — the projection family in pictures, and the download links
