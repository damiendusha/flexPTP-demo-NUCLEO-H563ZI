#!/usr/bin/env bash
#
# Build script for flexPTP-demo-NUCLEO-H743ZI
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
        -h|--help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  -c, --clean         Clean build directory before building"
            echo "  -b, --build-dir DIR Specify custom build directory (default: build)"
            echo "  -t, --build-type T  Build type: Debug, Release, RelWithDebInfo (default: Debug)"
            echo "  --target TARGET     CMake build target (default: flexptp-demo)"
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

# Step 3: Add toolchain to PATH
export PATH="${TOOLCHAIN_DIR}/bin:${PATH}"
info "Using compiler: $(arm-none-eabi-gcc --version | head -n 1)"

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
