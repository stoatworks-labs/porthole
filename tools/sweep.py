"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where the
warp is actually doing something, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

Two things about this plugin in particular that will fool you:

  * **The baseline must not be rectilinear.** At Projection = 0 the map is the
    identity by construction, so Field of View, Fit, Zoom and Quality all
    correctly do nothing and every one of them reads dead. The baseline below
    sits at equidistant for that reason.

  * **Quality is a real control with a small effect.** It changes only the
    pixels the warp is minifying, which on the geometry card is the fine
    checker band. Judged by "how many pixels moved" it will always look weaker
    than the rest; that is honest rather than a fault.
"""
import subprocess, zlib, struct, sys, tempfile

SC = tempfile.mkdtemp(prefix="phsweep")

# A baseline where the warp is genuinely bending something, so that nothing
# reads dead merely because the thing it modifies is switched off.
BASE = {
    "Projection": 0.5,      # equidistant -- NOT 0, which is the identity
    "Field of View": 0.6,
    "Defish": 0,
    "Chromatic": 0.3,
    "Fit": 0,
    "Centre X": 0.5,
    "Centre Y": 0.5,
    "Zoom": 0.5,
    "Edges": 2,             # clamp, so edge pixels carry picture rather than nothing
    "Quality": 1,
}

# Options are discrete; sweep them across their real element range. Everything
# else is a plain 0..1 float.
DISCRETE = {"Fit": (0, 3), "Edges": (0, 4), "Quality": (0, 2), "Defish": (0, 1)}

# A few controls need a baseline of their own to be visible at all.
#   Defish at the default Field of View is a mild change; give it a wide one.
#   Edges only matters where the warp samples outside the frame, which under a
#     diagonal fit it barely does -- a height fit and a zoom push it off-frame.
CONTEXT = {
    "Edges": {"Fit": 2, "Field of View": 0.8, "Zoom": 0.3},
    "Quality": {"Field of View": 0.85},
}


def render(path, overrides):
    args = ["./build/phtest", "--out", path, "--width", "1280", "--height", "720"]
    merged = dict(BASE)
    merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr)
        sys.exit(1)
    return open(path, "rb").read()


def pixels(png):
    i = 8
    idat = b""
    w = h = 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i + 4])[0]
        t = png[i + 4:i + 8]
        d = png[i + 8:i + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT":
            idat += d
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa)
    changed = 0
    total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / (n / 4) * 100, total / (n / 4)


names = subprocess.run(["./build/phtest", "--list"], capture_output=True, text=True).stdout
params = [" ".join(l.split()[1:-1]) for l in names.strip().splitlines()]

# The About block is a text field and browser buttons, declared last. They
# never touch a pixel, so sweeping them only buries a real dead control.
if "About" in params:
    params = params[:params.index("About")]

print(f"{'parameter':<16} {'pixels changed':>15} {'mean delta':>11}   verdict")
dead = []
for p in params:
    lo, hi = DISCRETE.get(p, (0.0, 1.0))
    context = CONTEXT.get(p, {})
    a = render(f"{SC}/a.png", {**context, p: lo})
    b = render(f"{SC}/b.png", {**context, p: hi})
    pct, mean = diff(a, b)
    ok = pct > 0.5
    if not ok:
        dead.append(p)
    print(f"{p:<16} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

print()
if dead:
    print("DEAD CONTROLS:", ", ".join(dead))
    sys.exit(1)
print(f"all {len(params)} parameters affect the output")
