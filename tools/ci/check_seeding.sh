#!/usr/bin/env bash
#
# Phase 8 acceptance criterion 2: the FreeRTOS project was seeded per the table in
# docs/bazel_migration.md §13.3 -- common files from the CURRENT tree, only stack-specific glue
# from master.
#
# Getting this wrong is not a build failure. master's copies of the common files are behind by
# roughly a thousand lines, including the F57, F62, GI-01 and GI-02 fixes, so seeding one of them
# from master ships a second firmware with those silently reverted.
#
# The check: for every file in the FreeRTOS project, if a file of the same name exists in the
# ThreadX project, the two must be IDENTICAL (it is common) or explicitly listed as per-project.
# A file that exists in both and differs is either a legitimate divergence that belongs in the
# list below, or a fix that has been reverted.
set -uo pipefail

TX=projects/flexptp_threadx_netx_demo
RT=projects/flexptp_freertos_lwip_demo

# Phase 9 note: files that are UNIFIED no longer appear in either project and so are not
# compared here at all -- //lib is the single copy, and there is nothing left to drift. This
# check therefore shrinks as unification proceeds, and what remains in it is the still-duplicated
# surface. See docs/bazel_migration.md §13.8.
#
# Files that legitimately differ between the two projects: the RTOS and stack bindings, the
# application entry point, and the configuration headers.
PER_PROJECT=(
    main.c
    cmds.c
    cmds.h
    flexptp_options.h

    # D15: the CMSIS-RTOS2 context tests are RE-DERIVED for each kernel, not translated. The
    # ISR half is common -- it is ST's wrapper's rule, and both wrappers have it -- but the
    # timer-thread half is ThreadX's own restriction (F80) and has no FreeRTOS counterpart.
    # A file that differs here is expected; a file that were identical would mean one of the
    # two firmwares is being tested against the other kernel's rules.
    test/test_rtos_context.cpp
)

is_per_project() {
    local f="$1"
    for p in "${PER_PROJECT[@]}"; do [ "$f" = "$p" ] && return 0; done
    return 1
}

fail=0
while IFS= read -r rel; do
    tx="${TX}/${rel}"
    [ -f "$tx" ] || continue                      # lwIP-only: nothing to compare against
    is_per_project "$rel" && continue
    if ! diff -q "$tx" "${RT}/${rel}" >/dev/null 2>&1; then
        echo "  DIVERGED  ${rel}"
        echo "            common to both projects but not identical -- either seeded from"
        echo "            master (reverting fixes) or edited in one project only."
        fail=1
    fi
done < <(cd "$RT" && find . -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.s' -o -name '*.S' -o -name '*.ld' \) | sed 's|^\./||')

# Invariants that hold ACROSS the two projects even in files that legitimately differ.
#
# main.c is per-project -- different RTOS, different stack, different bring-up -- so the
# whole-file comparison above deliberately skips it. That would let these drift silently, and
# one of them is a setting whose mismatch is invisible on both sides.
check_same() {
    local what="$1" pattern="$2" a b
    a=$(grep -hoE "$pattern" "${TX}/main.c" 2>/dev/null | head -1)
    b=$(grep -hoE "$pattern" "${RT}/main.c" 2>/dev/null | head -1)
    if [ -z "$a" ] || [ -z "$b" ]; then
        echo "  MISSING   ${what}: not found in one or both projects"
        fail=1
    elif [ "$a" != "$b" ]; then
        echo "  DIVERGED  ${what}"
        echo "            ${TX}: ${a}"
        echo "            ${RT}: ${b}"
        fail=1
    else
        echo "  ok        ${what}: ${a}"
    fi
}

# The initial PTP profile. Both firmwares must boot into the same one.
#
# PTP has no handshake, so a profile mismatch between board and master is SILENT on both sides
# -- see the project's ptp_profiles.md. 'default' is plain IEEE 1588 over UDP/IPv4 with E2E
# delay measurement: the preset that survives ordinary switches, and the one a stock ptp4l with
# no config file matches. 'gPTP' does not, because its destination MAC is in the 802.1D
# reserved range conformant bridges must not forward -- which is exactly how it presented when
# the FreeRTOS project was first restored: PTP_START passed, SLAVE never arrived.
check_same "initial PTP profile" 'FLEXPTP_INITIAL_PROFILE \(\"[a-zA-Z0-9]+\"\)'

if [ "$fail" -ne 0 ]; then
    echo
    echo "Two things this can mean:"
    echo "  DIVERGED on a common file -- seed it from ${TX}, not from master, or the fixes"
    echo "    master predates are being reverted. See docs/bazel_migration.md §13.3."
    echo "  DIVERGED on a cross-project invariant -- the two firmwares must agree on it."
    exit 1
fi
echo "Seeding rule holds: common files match, and the cross-project invariants agree."
