#!/usr/bin/env bash
#
# The full verification matrix, in one command: both firmwares, release and debug, each through
# the host suites, the S8 checks and every scenario on the board.
#
#   tools/hil/matrix.sh                         # everything: ~4.5 h with the board attached
#   tools/hil/matrix.sh --build-only            # host suites, the four images, their build IDs
#   tools/hil/matrix.sh --configs opt --firmwares threadx --scenarios health   # a quick pass
#
# Why both configurations. `bazel build` with no --config is fastbuild (-O0 -g): what a
# developer flashes, and NEITHER of the two that matter. Release is opt (-O2 -g0), and two of this
# port's defects only existed above -O0 (F62, a HardFault from a byte access the silicon rejects;
# F70, a bounded wait the compiler deleted). Debug is dbg (-O0 -g3 -DDEBUG), and DEBUG is what
# turns on the ThreadX software stack check and NetX Duo's API error checking -- error paths a
# release build has compiled out. Each catches what the other cannot, so a change is verified on
# both. See projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md.
#
# Every result lands in --out (default build/matrix-<date>): each image as <config>-<firmware>.elf
# with its build ID and size in images.txt, each run's console log and JSON report, and
# summary.txt. Keep the directory: the ELFs are what `swd.py` needs if anything has to be looked
# at afterwards, and the build IDs are what the board's results are tied to.
#
# Needs: the board on /dev/ttyACM0 and the host bench installed once with
# `sudo tools/hil/privileged/install.sh` (DHCP and the ptp4l units, sudo-free afterwards), and an
# OpenOCD the harness can find -- see tools/hil/README.md, "Requirements".
#
# Exit status: 0 if every step passed; 1 otherwise. FreeRTOS's S8 is "incomplete" (exit 3) by
# design -- its osinfo does not enumerate threads, so `threads` SKIPs -- and that one skip is
# accepted; any other skip is not.
#
set -uo pipefail

CONFIGS="opt dbg"
FIRMWARES="threadx freertos"
STAGE="S8"
SCENARIOS="all"
BUILD_ONLY=0
SKIP_HOST=0
OUT="build/matrix-$(date +%Y%m%d-%H%M)"

usage() { sed -n '2,35p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --configs)    CONFIGS="${2:?}"; shift ;;
        --firmwares)  FIRMWARES="${2:?}"; shift ;;
        --stage)      STAGE="${2:?}"; shift ;;
        --scenarios)  SCENARIOS="${2-}"; shift ;;    # a preset or scenario names, or "" for none
        --out)        OUT="${2:?}"; shift ;;
        --build-only) BUILD_ONLY=1 ;;
        --skip-host)  SKIP_HOST=1 ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 2
mkdir -p "$OUT" || exit 2
SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"
failed=0

note() { echo "$*" | tee -a "$SUMMARY"; }
step() { echo; echo "######## $*"; }

declare -A TARGET=(
    [threadx]=//projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flexptp-demo
    [freertos]=//projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo:flexptp-demo-freertos
)
declare -A ELF=(
    [threadx]=bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.elf
    [freertos]=bazel-bin/projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo/flexptp-demo-freertos.elf
)

note "matrix: configs '$CONFIGS', firmwares '$FIRMWARES', stage '$STAGE', scenarios '${SCENARIOS:-none}'"
note "tree: $(git rev-parse --short HEAD)$(git diff --quiet HEAD -- . 2>/dev/null || echo ' + uncommitted changes')"

# --- 1. Host suites, in each configuration ---------------------------------------------------
# The host tests compile with the host compiler, so -O2 and -O0 genuinely build different code:
# undefined behaviour that one optimiser tolerates the other may not.
if [[ $SKIP_HOST -eq 0 ]]; then
    for cfg in fastbuild $CONFIGS; do
        step "host suites, $cfg"
        flag=$([[ $cfg == fastbuild ]] && echo "" || echo "--config=$cfg")
        if bazel test $flag //... > "$OUT/host-$cfg.log" 2>&1; then
            note "PASS  host suites ($cfg): $(grep -oE 'Executed [0-9]+ out of [0-9]+ tests.*' "$OUT/host-$cfg.log" | tail -1)"
        else
            note "FAIL  host suites ($cfg): see $OUT/host-$cfg.log"; failed=1
        fi
    done
fi

# --- 2. The images ---------------------------------------------------------------------------
RE="$(python3 -c "import sys; sys.path.insert(0, 'tools/hil'); import swd; print(swd.find_arm_tool('readelf') or '')")"
SIZE="${RE%readelf}size"
for cfg in $CONFIGS; do
    step "build, $cfg"
    targets=""
    for fw in $FIRMWARES; do targets+=" ${TARGET[$fw]}"; done
    if ! bazel build --config="$cfg" $targets > "$OUT/build-$cfg.log" 2>&1; then
        note "FAIL  build ($cfg): see $OUT/build-$cfg.log"; failed=1; continue
    fi
    for fw in $FIRMWARES; do
        img="$OUT/$cfg-$fw.elf"
        cp -f "${ELF[$fw]}" "$img" && chmod u+w "$img"
        id="$("$RE" -n "$img" | awk '/Build ID/ {print $3}')"
        sz="$("$SIZE" "$img" | awk 'NR == 2 {print "text " $1 ", data " $2 ", bss " $3}')"
        echo "$cfg-$fw  $id  $sz" >> "$OUT/images.txt"
        note "      image $cfg-$fw: build ID ${id:0:8}, $sz"
    done
done
[[ $BUILD_ONLY -eq 1 ]] && { note "build only: done"; exit $failed; }

# --- 3. On the board --------------------------------------------------------------------------
# The default grandmaster must be running: S8 syncs to it, and every scenario returns to it.
systemctl start flexptp-ptp4l-default 2>/dev/null || note "note: could not start flexptp-ptp4l-default (bench not installed?)"
sleep 3

for cfg in $CONFIGS; do
    for fw in $FIRMWARES; do
        img="$OUT/$cfg-$fw.elf"
        [[ -f $img ]] || continue
        name="$cfg-$fw"

        step "$name: $STAGE"
        tools/hil/run.py --flash "$img" --elf "$img" --firmware "$fw" --stage "$STAGE" \
            --save "$OUT/$name-$STAGE.json" > "$OUT/$name-$STAGE.log" 2>&1
        rc=$?
        tally="$(sed 's/\x1b\[[0-9;]*m//g' "$OUT/$name-$STAGE.log" | grep -E '^[0-9]+ passed' | tail -1)"
        skips="$(sed 's/\x1b\[[0-9;]*m//g' "$OUT/$name-$STAGE.log" | awk '$1 == "SKIP" {print $2}' | sort -u | tr '\n' ' ')"
        if [[ $rc -eq 0 ]] || [[ $rc -eq 3 && $fw == freertos && "$skips" == "threads " ]]; then
            note "PASS  $name $STAGE: $tally${skips:+ (skipped: $skips)}"
        else
            note "FAIL  $name $STAGE: exit $rc, $tally${skips:+ (skipped: $skips)}; see $OUT/$name-$STAGE.log"
            failed=1
            continue    # no point running an hour of scenarios on a board that failed S8
        fi

        [[ -n $SCENARIOS ]] || continue
        step "$name: scenarios $SCENARIOS"
        tools/hil/scenario.py --firmware "$fw" --elf "$img" $SCENARIOS \
            --save "$OUT/$name-scenarios.json" > "$OUT/$name-scenarios.log" 2>&1
        rc=$?
        tally="$(sed 's/\x1b\[[0-9;]*m//g' "$OUT/$name-scenarios.log" | grep -E '^[0-9]+ passed, [0-9]+ failed in' | tail -1)"
        if [[ $rc -eq 0 ]]; then
            note "PASS  $name scenarios: $tally"
        else
            note "FAIL  $name scenarios: exit $rc, $tally; see $OUT/$name-scenarios.log"
            failed=1
        fi
    done
done

echo
note "$( [[ $failed -eq 0 ]] && echo 'MATRIX PASSED' || echo 'MATRIX FAILED' ) -- results in $OUT"
exit $failed
