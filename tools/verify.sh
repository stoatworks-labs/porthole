#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# Three things get checked, and they fail in different ways:
#
#   --probe       the GLSL copy of the lens maths against the C++ copy, across
#                 the whole parameter space. Catches a typo in either.
#   --roundtrip   that Defish really is the inverse of the fish rather than
#                 something that merely looks like it.
#   sweep.py      that no control is silently dead.
#
# The probe and the round trip both decline to answer where the picture cannot
# support the question -- a radius that falls outside the frame, or a region the
# fish has already destroyed. Those are reported as SKIP, not as passes.
set -uo pipefail

cd "$(dirname "$0")/.."

if [[ ! -x build/phtest ]]; then
	echo "build/phtest not found. Run:"
	echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
	exit 1
fi

# The parameter plumbing first: it needs no GPU, it takes a moment, and it is
# the half an external user actually got stuck on (vertigo issue #2).
echo "== presets: every factory preset survives every host behaviour"
if ./build/phtest --presets | tail -1; then
	:
else
	failures+=("presets")
fi

probe_pass=0; probe_fail=0
trip_pass=0; trip_fail=0; trip_skip=0
failures=()

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

echo "== shaders: every one through a real GLSL compiler"
if ! shaders_compile; then
	failures+=("shaders")
fi
echo

echo "== probe: GLSL against C++, over fit x projection x field of view x direction"
for fit in 0 1 2 3; do
	for projection in 0.0 0.25 0.5 0.75 1.0; do
		for fov in 0.3 0.6 0.9; do
			for defish in 0 1; do
				if ./build/phtest --probe \
					--set "Fit=$fit" --set "Projection=$projection" \
					--set "Field of View=$fov" --set "Defish=$defish" >/dev/null 2>&1; then
					probe_pass=$((probe_pass + 1))
				else
					probe_fail=$((probe_fail + 1))
					failures+=("probe fit=$fit projection=$projection fov=$fov defish=$defish")
				fi
			done
		done
	done
done
echo "   $probe_pass passed, $probe_fail failed"

echo "== roundtrip: fish then defish, over fit x projection x field of view"
for fit in 0 1 2 3; do
	for projection in 0.25 0.5 0.75 1.0; do
		for fov in 0.3 0.6 0.9 1.0; do
			./build/phtest --roundtrip \
				--set "Fit=$fit" --set "Projection=$projection" \
				--set "Field of View=$fov" >/dev/null 2>&1
			case $? in
				0) trip_pass=$((trip_pass + 1)) ;;
				2) trip_skip=$((trip_skip + 1)) ;;
				*)
					trip_fail=$((trip_fail + 1))
					failures+=("roundtrip fit=$fit projection=$projection fov=$fov")
					;;
			esac
		done
	done
done
echo "   $trip_pass passed, $trip_fail failed, $trip_skip too destroyed to judge"

echo "== sweep: no control silently dead"
if python3 tools/sweep.py > /tmp/porthole-sweep.txt 2>&1; then
	echo "   all parameters affect the output"
	sweep_ok=1
else
	echo "   *** dead controls, see /tmp/porthole-sweep.txt"
	tail -4 /tmp/porthole-sweep.txt
	sweep_ok=0
	failures+=("sweep")
fi

echo
if (( ${#failures[@]} == 0 )); then
	echo "all checks passed"
	exit 0
fi

echo "FAILURES:"
printf '  %s\n' "${failures[@]}"
exit 1
