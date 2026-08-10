#!/usr/bin/env bash
#
# Build script for flexPTP-demo-NUCLEO-H563ZI
#

set -euo pipefail

# Directory locations
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLCHAINS_DIR="${PROJECT_ROOT}/toolchains"
TOOLCHAIN_NAME="arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi"
TOOLCHAIN_DIR="${TOOLCHAINS_DIR}/${TOOLCHAIN_NAME}"
TOOLCHAIN_ARCHIVE="${TOOLCHAINS_DIR}/${TOOLCHAIN_NAME}.tar.xz"
TOOLCHAIN_URL="https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi.tar.xz"
BUILD_DIR="${PROJECT_ROOT}/build"
BUILD_TYPE="Debug"
CLEAN_BUILD=0
TARGET="flexptp-demo"

# OpenOCD. The STM32H5 needs ST's fork: target/stm32h5x.cfg declares "flash bank ... stm32h5x",
# which names a compiled-in flash driver, not tcl. Stock OpenOCD 0.12.0 has no such driver and
# rejects the declaration, so copying ST's .cfg files onto a distro build does not work -- the
# binary itself must have H5 support.
OPENOCD_REPO="https://github.com/STMicroelectronics/OpenOCD.git"
OPENOCD_TAG="openocd-cubeide-v2.1.0"
OPENOCD_SRC_DIR="${TOOLCHAINS_DIR}/openocd-src"
OPENOCD_DIR="${TOOLCHAINS_DIR}/openocd"
WITH_OPENOCD=0

# Formatting helpers
BOLD='\033[1m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

info() {
    echo -e "${BLUE}${BOLD}[INFO]${NC} $*"
}

success() {
    echo -e "${GREEN}${BOLD}[SUCCESS]${NC} $*"
}

warn() {
    echo -e "${YELLOW}${BOLD}[WARNING]${NC} $*"
}

error() {
    echo -e "${RED}${BOLD}[ERROR]${NC} $*" >&2
}

# Parse command line options
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--clean)
            CLEAN_BUILD=1
            shift
            ;;
        -b|--build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -t|--build-type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --target)
            TARGET="$2"
            shift 2
            ;;
        --with-openocd)
            WITH_OPENOCD=1
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  -c, --clean         Clean build directory before building"
            echo "  -b, --build-dir DIR Specify custom build directory (default: build)"
            echo "  -t, --build-type T  Build type: Debug, Release, RelWithDebInfo (default: Debug)"
            echo "  --target TARGET     CMake build target (default: flexptp-demo)"
            echo "  --with-openocd      Build ST's OpenOCD fork into toolchains/openocd."
            echo "                      Required to flash or debug an STM32H5 -- upstream"
            echo "                      OpenOCD 0.12.0 cannot program this part."
            echo "  -h, --help          Show this help message"
            exit 0
            ;;
        *)
            error "Unknown argument: $1"
            echo "Use $0 --help for usage information."
            exit 1
            ;;
    esac
done

# Check the host packages OpenOCD's autotools build needs. Report everything that is missing
# at once, with package names -- failing halfway through ./bootstrap with an autoconf error is
# a miserable way to find out.
check_openocd_prereqs() {
    local missing=()

    # NOTE: test for libtoolize, not libtool. The Debian/Ubuntu "libtool" package installs
    # /usr/bin/libtoolize but no /usr/bin/libtool, so checking the obvious name reports a
    # failure on a machine that is perfectly ready to build.
    for tool in git make gcc autoconf automake libtoolize pkg-config; do
        command -v "${tool}" >/dev/null 2>&1 || missing+=("${tool}")
    done

    if ! pkg-config --exists libusb-1.0 2>/dev/null; then
        missing+=("libusb-1.0 development headers")
    fi

    if [ ${#missing[@]} -gt 0 ]; then
        error "Cannot build OpenOCD -- missing: ${missing[*]}"
        echo ""
        echo "  Debian/Ubuntu: sudo apt install git build-essential autoconf automake libtool pkg-config libusb-1.0-0-dev"
        echo "  Fedora:        sudo dnf install git gcc make autoconf automake libtool pkgconf-pkg-config libusb1-devel"
        echo "  Arch:          sudo pacman -S git base-devel libtool pkgconf libusb"
        echo ""
        echo "These need root, so this script will not install them for you."
        exit 1
    fi
}

provision_openocd() {
    if [ -x "${OPENOCD_DIR}/bin/openocd" ]; then
        info "OpenOCD already installed at ${OPENOCD_DIR}"
        return
    fi

    check_openocd_prereqs

    if [ ! -d "${OPENOCD_SRC_DIR}/.git" ]; then
        info "Cloning ST's OpenOCD fork at ${OPENOCD_TAG}..."
        mkdir -p "${TOOLCHAINS_DIR}"
        git clone --branch "${OPENOCD_TAG}" --depth 1 "${OPENOCD_REPO}" "${OPENOCD_SRC_DIR}"
    else
        info "Reusing ${OPENOCD_SRC_DIR}, checking out ${OPENOCD_TAG}..."
        git -C "${OPENOCD_SRC_DIR}" fetch --depth 1 origin "refs/tags/${OPENOCD_TAG}:refs/tags/${OPENOCD_TAG}" 2>/dev/null || true
        git -C "${OPENOCD_SRC_DIR}" checkout -q "${OPENOCD_TAG}"
    fi

    # ./bootstrap pulls the jimtcl and libjaylink submodules and generates configure
    if [ ! -x "${OPENOCD_SRC_DIR}/configure" ]; then
        info "Bootstrapping OpenOCD (fetches jimtcl and libjaylink)..."
        ( cd "${OPENOCD_SRC_DIR}" && ./bootstrap )
    fi

    # --disable-werror is not optional. OpenOCD turns -Werror on by default for git
    # checkouts, and this tree (0.12.0-era) does not compile clean under current GCC -- 15.2
    # fails it in angie.c, ulink.c and ti_icdi_usb.c, none of which this project uses. Without
    # the flag the build dies partway through with "all warnings being treated as errors".
    info "Configuring OpenOCD (prefix ${OPENOCD_DIR})..."
    ( cd "${OPENOCD_SRC_DIR}" && ./configure --prefix="${OPENOCD_DIR}" --enable-stlink --disable-werror )

    local jobs
    jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    info "Building OpenOCD using ${jobs} parallel jobs..."
    make -C "${OPENOCD_SRC_DIR}" -j"${jobs}"
    make -C "${OPENOCD_SRC_DIR}" install

    success "OpenOCD installed: $("${OPENOCD_DIR}/bin/openocd" --version 2>&1 | head -n 1)"
    echo ""
    warn "On Linux, flashing without sudo needs ST-Link udev rules:"
    echo "    sudo cp ${OPENOCD_SRC_DIR}/contrib/60-openocd.rules /etc/udev/rules.d/"
    echo "    sudo udevadm control --reload-rules && sudo udevadm trigger"
    echo ""
}

# Step 0: Build OpenOCD if asked
if [ "${WITH_OPENOCD}" -eq 1 ]; then
    provision_openocd
fi

# Step 1: Initialize / update git submodules if needed
info "Checking git submodules..."
if [ -d "${PROJECT_ROOT}/.git" ]; then
    git -C "${PROJECT_ROOT}" submodule update --init --recursive
fi

# Step 2: Ensure toolchain is downloaded and extracted
if [ ! -f "${TOOLCHAIN_DIR}/bin/arm-none-eabi-gcc" ]; then
    mkdir -p "${TOOLCHAINS_DIR}"
    
    if [ ! -f "${TOOLCHAIN_ARCHIVE}" ]; then
        info "Downloading ARM GNU Toolchain 15.3.rel1..."
        info "URL: ${TOOLCHAIN_URL}"
        if command -v curl >/dev/null 2>&1; then
            curl -L --fail --progress-bar "${TOOLCHAIN_URL}" -o "${TOOLCHAIN_ARCHIVE}"
        elif command -v wget >/dev/null 2>&1; then
            wget --progress=bar:force -O "${TOOLCHAIN_ARCHIVE}" "${TOOLCHAIN_URL}"
        else
            error "Neither curl nor wget is available. Please install curl or wget."
            exit 1
        fi
    fi

    info "Extracting toolchain into ${TOOLCHAIN_DIR}..."
    tar -xf "${TOOLCHAIN_ARCHIVE}" -C "${TOOLCHAINS_DIR}"
    success "Toolchain extracted successfully."
else
    info "ARM GNU Toolchain already installed at ${TOOLCHAIN_DIR}"
fi

# Step 3: Add toolchains to PATH
export PATH="${TOOLCHAIN_DIR}/bin:${PATH}"
info "Using compiler: $(arm-none-eabi-gcc --version | head -n 1)"

# Put OpenOCD on PATH whenever it is present, not only when it was just built, so that
# "./build.sh && openocd -f board/st_nucleo_h5.cfg ..." works in the same shell.
if [ -x "${OPENOCD_DIR}/bin/openocd" ]; then
    export PATH="${OPENOCD_DIR}/bin:${PATH}"
fi

# Step 4: Handle clean build if requested
if [ "${CLEAN_BUILD}" -eq 1 ] && [ -d "${BUILD_DIR}" ]; then
    info "Cleaning build directory: ${BUILD_DIR}..."
    rm -rf "${BUILD_DIR}"
fi

# Step 5: Configure with CMake
info "Configuring project with CMake (Build Type: ${BUILD_TYPE})..."
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

# Step 6: Build target
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
info "Building target '${TARGET}' using ${JOBS} parallel jobs..."
cmake --build "${BUILD_DIR}" --target "${TARGET}" -j"${JOBS}"

# Step 7: Verify outputs
success "Build completed successfully!"
echo ""
echo "Output files in ${BUILD_DIR}:"
if ls "${BUILD_DIR}/${TARGET}".* >/dev/null 2>&1; then
    ls -lh "${BUILD_DIR}/${TARGET}".*
fi

echo ""
if [ -x "${OPENOCD_DIR}/bin/openocd" ]; then
    echo "To flash:"
    echo "    ${OPENOCD_DIR}/bin/openocd -f board/st_nucleo_h5.cfg \\"
    echo "        -c init -c halt -c \"program ${BUILD_DIR}/${TARGET}.elf verify reset\" -c exit"
else
    echo "No OpenOCD in toolchains/. Run '$0 --with-openocd' to build ST's fork;"
    echo "upstream OpenOCD 0.12.0 cannot program an STM32H5."
fi
