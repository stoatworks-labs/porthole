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
