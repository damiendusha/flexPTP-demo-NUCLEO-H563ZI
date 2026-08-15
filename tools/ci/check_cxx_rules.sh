#!/usr/bin/env bash
#
# The C++ rules in Src/cpp_support.cpp, enforced against the linked image.
#
# Those rules -- no dynamic allocation, no exceptions, no RTTI -- were a comment. Two of them
# are already enforced by compiler flags (-fno-exceptions, -fno-rtti); the allocation rule was
# not enforced by anything, and "no new" is exactly the sort of rule that holds until somebody
# adds a std::vector to a unit test's worth of production code (finding CX-07).
#
# Checked against the IMAGE rather than the sources, for the same reason `cpp_ctor` in
# tools/hil/ asks the running board whether static constructors ran: a rule about what the
# firmware contains is a question for the firmware, not for grep.
#
# Usage: tools/ci/check_cxx_rules.sh <elf> [nm]
set -euo pipefail

ELF="${1:?usage: $0 <elf> [nm]}"
NM="${2:-arm-none-eabi-nm}"

if ! command -v "${NM}" >/dev/null 2>&1; then
    # Fall back to the toolchain build.sh fetches.
    ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    NM="${ROOT}/toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-nm"
fi

# Mangled names, because that is what is in the symbol table:
#   _Znwj / _Znaj            operator new / new[]        (32-bit unsigned size)
#   _ZdlPv / _ZdaPv          operator delete / delete[]
#   __cxa_throw              a throw expression
#   __cxa_allocate_exception ditto, the allocation half
#   _ZTVN10__cxxabiv1*       RTTI type_info vtables
FORBIDDEN='_Znwj|_Znaj|_ZdlPv|_ZdaPv|__cxa_throw|__cxa_allocate_exception|_ZTVN10__cxxabiv1'

found="$("${NM}" "${ELF}" | grep -E " (T|t|W|w|D|d|B|b) .*(${FORBIDDEN})" || true)"

if [ -n "${found}" ]; then
    echo "ERROR: the linked image violates the C++ rules in Src/cpp_support.cpp:"
    echo "${found}"
    echo ""
    echo "  No dynamic allocation, no exceptions, no RTTI. There is no RTOS heap to allocate"
    echo "  from, and operator new would fall through to newlib malloc -> _sbrk, which on this"
    echo "  firmware reports and halts when exhausted (see Src/sysmem.c)."
    exit 1
fi

echo "C++ rules hold: no operator new/delete, no exceptions, no RTTI in $(basename "${ELF}")"
