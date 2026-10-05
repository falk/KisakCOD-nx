#!/usr/bin/env bash
# Build the deko9 self-test NRO and run it under an emulator until its verdict.
#   tools/deko3d/selftest/run.sh [log]
#   LTO=1 builds and runs the LTO variant (KisakCOD-deko9-selftest-lto).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
log=${1:-"$root/build/deko9-selftest.log"}
dkp=${DEVKITPRO:?set DEVKITPRO}
# The shader-compiler libraries come from the switch-sp CMake build.
export DEPS_BUILD=${KISAK_DEPS_BUILD:-"$root/build/switch-sp"}
cmake_env=(env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS)
[ -f "$DEPS_BUILD/CMakeCache.txt" ] ||
    (cd "$root" && "${cmake_env[@]}" cmake --preset switch-sp -B "$DEPS_BUILD" >/dev/null)
"${cmake_env[@]}" cmake --build "$DEPS_BUILD" --target uam mojoshader >/dev/null
name=KisakCOD-deko9-selftest
[ "${LTO:-0}" = 1 ] && name=$name-lto
env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS \
    make -C "$here" DEVKITPRO=$dkp KISAK_SWITCH_LOG_HOST="${KISAK_SWITCH_LOG_HOST:-}" \
    DEKO3D_DEBUG="${DEKO3D_DEBUG:-1}" LTO="${LTO:-0}" >/dev/null
# An emulator loader needs the NACP/icon packaging.
"$dkp/tools/bin/nacptool" --create "deko9 selftest" kisak-switch 0.1.0 "$here/$name.nacp" >/dev/null
"$dkp/tools/bin/elf2nro" "$here/$name.elf" "$here/$name.nro" \
    --icon="$dkp/libnx/default_icon.jpg" --nacp="$here/$name.nacp" >/dev/null
mkdir -p "$(dirname "$log")"
# A previous run's log must never satisfy the checks below: remove it first,
# and fail if the emulator run itself did not start (e.g. shared emulator busy).
rm -f "$log"
# The emulator runner is a local script: KISAK_EMULATOR_RUNNER=<script> takes
# <nro> <log> <until-regex> <timeout-s>, streams the emulator's output into
# <log> and stops once the regex matches.
sw=$root/scripts/platform/switch
runner=${KISAK_EMULATOR_RUNNER:-$sw/run-ryubing-until.sh}
[ -x "$runner" ] || { echo "FAIL:DEKO9_SELFTEST_RUN no emulator runner (set KISAK_EMULATOR_RUNNER)"; exit 1; }
rc=0
"$runner" "$here/$name.nro" "$log" \
    'PASS:DEKO9_SELFTEST$|FAIL:DEKO9_SELFTEST$|Unhandled exception|stalled 4s' 360 || rc=$?
if [ ! -s "$log" ]; then
    echo "FAIL:DEKO9_SELFTEST_RUN emulator run produced no log (rc=$rc)"
    exit 1
fi
grep -aE 'DEKO9_SELFTEST|DEKO9 gpupass|DEKO9 fsr frames|DEKO9 taau frames' "$log" | sed 's/.*OutputDebugString: //'
grep -aqE 'PASS:DEKO9_SELFTEST$' "$log"
# Per-pass GPU timing reported for the marked present loop.
grep -aqE 'DEKO9 gpupass frames=60 total=.* lit=.* hud2d=.* present=' "$log" || {
    echo "FAIL:DEKO9_SELFTEST_GPUPASS (no gpupass line with lit/hud2d/present)"
    exit 1
}
# Upscaler GPU timing reported per r_fsrMode mode.
for mode in sgsr bilinear_rcas bilinear; do
    grep -aqE "DEKO9 fsr frames=60 gpu=[0-9.]+ms .* mode=$mode samples=[1-9]" "$log" || {
        echo "FAIL:DEKO9_SELFTEST_FSR_TIMING_$mode (no fsr timing line for mode $mode)"
        exit 1
    }
done
# Dynamic-resolution scene upscale (Deko9_UpscaleSurface) timed per mode.
for mode in sgsr bilinear_rcas bilinear; do
    grep -aqE "DEKO9 fsr frames=60 gpu=[0-9.]+ms .* mode=$mode samples=[1-9][0-9]* path=scene" "$log" || {
        echo "FAIL:DEKO9_SELFTEST_DYNRES_TIMING_$mode (no path=scene fsr timing line for mode $mode)"
        exit 1
    }
done
# TAAU resolve (Deko9_TaauResolve) timed by the device.
grep -aqE 'DEKO9 taau frames=60 gpu=[0-9.]+ms .* samples=[1-9]' "$log" || {
    echo "FAIL:DEKO9_SELFTEST_TAAU_TIMING (no taau timing line with samples)"
    exit 1
}
# Draw census (r_deko9DrawCensus): the hud2d quad row writes 640x360 samples
# per draw, and the lights line counts the 2 lights in view / 1 drawn light.
grep -aE 'DEKO9 dcensus label=selftest' "$log" | sed 's/.*OutputDebugString: //'
grep -aE 'DEKO9 dcensus label=selftest rank=[0-9]+ pass=hud2d mat=selftest_quad ' "$log" |
    sed -E 's/.* draws=([0-9.]+) prims=[0-9.]+ px=([0-9]+) .*/\1 \2/' |
    awk '{ if ($1 > 0 && ($2 / $1) > 230400 * 0.99 && ($2 / $1) < 230400 * 1.01) ok = 1 }
         END { exit ok ? 0 : 1 }' || {
    echo "FAIL:DEKO9_SELFTEST_DRAW_CENSUS (no hud2d selftest_quad row with 230400 samples per draw)"
    exit 1
}
grep -aqE 'DEKO9 dcensus label=selftest rank=[0-9]+ pass=lights mat=selftest_light ' "$log" &&
    grep -aqE 'DEKO9 dcensus label=selftest lights inview=2.00 partitions=1.00 drawn=1.00 draws=1.0 unowned_draws=0.0' "$log" || {
    echo "FAIL:DEKO9_SELFTEST_DRAW_CENSUS_LIGHTS (lights row or light counts missing)"
    exit 1
}
# gdraws= counts deko3d draw packets: the decal pass's one ranged call per
# frame (2 ranges) is 1 API draw and 2 packets.
grep -aE 'DEKO9 dcensus label=selftest pass=decal ' "$log" |
    sed -E 's/.* draws=([0-9.]+) .* gdraws=([0-9.]+).*/\1 \2/' |
    awk '{ if ($1 > 0.9 && $1 < 1.1 && $2 > 1.9 * $1 && $2 < 2.1 * $1) ok = 1 } END { exit ok ? 0 : 1 }' || {
    echo "FAIL:DEKO9_SELFTEST_DRAW_CENSUS_GDRAWS (no decal pass line with 1 draw and 2 gdraws per frame)"
    exit 1
}
echo "PASS:DEKO9_SELFTEST_DRAW_CENSUS"
# Every draw recorded under r_deko9FaultTrace passed the black box's VA check.
if grep -aq 'FAIL:DEKO9_DRAW_VA' "$log"; then
    grep -a 'FAIL:DEKO9_DRAW_VA' "$log" | head -3
    echo "FAIL:DEKO9_SELFTEST_DRAW_VA (a recorded draw's VA is outside every heap)"
    exit 1
fi
echo "PASS:DEKO9_SELFTEST_DRAW_VA"
