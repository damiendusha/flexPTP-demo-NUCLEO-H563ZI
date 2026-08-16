#!/usr/bin/env bash
#
# Asserts that the OpenOCD Bazel selected can actually program this board.
#
# A build that "succeeded" does not imply a usable flasher. Two ways it can be useless:
#
#   - it is upstream OpenOCD rather than ST's fork, and has no compiled-in stm32h5x flash
#     driver, so target/stm32h5x.cfg's flash bank declaration is rejected at flash time
#   - it was built on a host without libftdi/hidapi, so some adapters silently dropped out of
#     the build. Harmless for this board -- the Nucleo's onboard ST-Link speaks libusb -- but
#     "the build succeeded" is not evidence either way, so it is asserted rather than assumed.
#
# Runs without a board attached, so it gates every push.
set -uo pipefail

OPENOCD="${1:?usage: $0 <openocd>}"
fail=0

check() {
    local what="$1" pattern="$2" haystack="$3"
    if grep -qi -- "$pattern" <<<"$haystack"; then
        echo "  ok    ${what}"
    else
        echo "  FAIL  ${what} (no '${pattern}')"
        fail=1
    fi
}

banner="$("${OPENOCD}" --version 2>&1 || true)"
echo "${banner}" | head -1
check "is ST's fork" "STMicroelectronics" "${banner}"

# `openocd --help` lists the compiled-in flash drivers and adapters.
help="$("${OPENOCD}" --help 2>&1 || true)"
strings_out="$(strings "${OPENOCD}" 2>/dev/null || true)"
check "has the stm32h5x flash driver" "stm32h5x" "${help}${strings_out}"
check "has the st-link adapter" "st-link" "${help}${strings_out}"

if [ "${fail}" -ne 0 ]; then
    echo
    echo "This OpenOCD cannot program an STM32H5. Build ST's fork with:"
    echo "    ./build.sh --with-openocd"
    exit 1
fi
echo "OpenOCD is usable for this board."
