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

# Files that legitimately differ between the two projects: the RTOS and stack bindings, the
# application entry point, and the configuration headers.
PER_PROJECT=(
    main.c
    cmds.c
    cmds.h
    flexptp_options.h
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

if [ "$fail" -ne 0 ]; then
    echo
    echo "Seed common files from ${TX}, not from master. See docs/bazel_migration.md §13.3."
    exit 1
fi
echo "Seeding rule holds: every common file matches the ThreadX project."
