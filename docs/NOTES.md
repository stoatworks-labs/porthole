# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*porthole — variable fisheye/defish lens projection warp as an FFGL effect for Resolume; PUBLIC MIT, verified offline by probe + round trip, and since 2026-08-02 run in Resolume on real content*

**Porthole** (started 2026-08-02) — a **lens projection warp**, not a bulge
effect, as an FFGL effect for Resolume Arena/Avenue.
`~/Projects/porthole`, **PUBLIC MIT**, github.com/stoatworks-labs/porthole,
v0.1.0, no release cut yet. Effect ID `PH01`.

**The design idea that governs everything:** it re-photographs the picture
through a different lens. The Projection control is **which lens, not how much** —
one continuous family `P_k(θ) = tan(kθ)/k | θ | sin(kθ)/k` that is smooth
through k=0 and lands exactly on rectilinear (+1), stereographic (+0.5),
equidistant (0), equisolid (−0.5), orthographic (−1).

Three properties **fall out** of that rather than being arranged, and breaking
any of them means the model is wrong:
- **Rectilinear is the identity** at any field of view.
- **Defish is the exact inverse** (same formula, source/destination swapped).
- **The frame stays full** — the reference radius is a fixed point,
  `warpRadius(1)==1`, so only the interior is redistributed.

No wet/dry mix on purpose: cross-fading two geometries double-exposes. The null
is Field of View = 0. **Chromatic aberration is deliberately independent of
distortion** (lateral CA is magnification-vs-wavelength; a rectilinear lens can
have it), so it is not covered by that null.

**The lens maths exists twice on purpose** — C++ in `Projection.cpp` for
readability/testing, GLSL in `Shaders.cpp` because it runs per pixel.
`phtest --probe` is what makes that safe: it feeds in a picture whose brightness
**is** the normalised radius, so the output pixel states as a number which source
radius the GPU sampled from, and compares it to the C++. **120 combinations agree
to 0.6 of an 8-bit level.** `phtest --roundtrip` closes to 0.001 levels over 48 of
64. `tools/sweep.py` proves none of the 10 controls is dead. `tools/verify.sh`
runs the lot (~3 min).

**Both measurements refuse to answer where the picture cannot support the
question** — this cost two false alarms while building it. A probe reading along
+x can only see radii that exist on that axis (only ~0.87 under a diagonal fit),
and defish legitimately wants source from beyond the frame. And where the fish
compressed past 2:1 the detail is genuinely gone. **The subtle half: the loss is
governed by the radius the inverse READS from, not the one it writes to** — gating
on the forward map's gradient at the output radius passes the whole frame and
measures the damage anyway.

**Run inside Resolume on real content** as of 2026-08-02 (Allan's own report, not
a session observation) — so the old "never loaded into Resolume, not once" is
retired. **v0.1.0 released 2026-08-02**: the tag-triggered CI publishes a macOS
universal .dmg, a Windows x64 setup.exe and both zips. **Windows compiles** —
the old "Windows never compiled" note is wrong, proven by a workflow_dispatch
run that built macOS + Windows + the NSIS installer green before the tag went
up. Dispatching that workflow is the cheap way to test an FFGL build without
publishing anything: the release job is gated on `refs/tags/v*`, so a manual
run builds and skips publication.

Still open: **nothing timed** (supersampling multiplies per-pixel cost up to
16×, unmeasured).

Traps in the repo's AGENTS.md — see [agents md convention](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_agents_md_convention.md). Built on
the CMake MODULE + FFGL submodule pattern from [resolume luma keyer](https://github.com/stoatworks-labs/resolume-luma-keyer/blob/main/docs/NOTES.md) (`resolume-luma-keyer`)
and [old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`); SDK traps in [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md).
**disclaimer scope** (working-practice note, kept in Claude memory) applies.
