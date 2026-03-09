#!/bin/bash
#
# Hydrogen Kernel build script
# Brought to you by z3rokwq @ pissarro-development
#

# Exit immediately if a command exits with a non-zero status.
set -e

# Function to display help message
show_help() {
    cat << EOF
Usage: $0 [OPTIONS] [CODENAME]

Build script for Hydrogen Kernel

OPTIONS:
    -h, --help      Show this help message and exit
    -c, --clean     Perform a full clean build (removes out directory)

ARGUMENTS:
    CODENAME        Device codename (default: pissarro)

EXAMPLES:
    $0                      # Build for default deviceZ
    $0 <codename>           # Build for specific device
    $0 -c                   # Clean build for default device
    $0 --clean <codename>   # Clean build for specific device

EOF
}

# Initial Setup
SECONDS=0
DATE=$(date '+%Y%m%d-%H%M')

# Default device
DEVICE="pissarro"
CLEAN_BUILD=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -*)
            echo "Error: Unknown option: $1"
            echo "Use -h or --help for usage information"
            exit 1
            ;;
        *)
            DEVICE="$1"
            shift
            ;;
    esac
done

DEFCONFIG="${DEVICE}_defconfig"
ZIPNAME="HydrogenKernel-${DEVICE}-${DATE}.zip"

echo -e "\nBuilding for device: $DEVICE\n"

# Toolchain Setup
CLANG_VERSION="clang-r530567b"
TC_DIR="$HOME/toolchains"
if [ ! -d "$TC_DIR/$CLANG_VERSION" ]; then
    echo -e "Toolchain not found, downloading AOSP clang...\n"
    git clone --depth=1 --branch=android15-qpr2-release https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86 "$TC_DIR/.temp"
    mv "$TC_DIR/.temp/$CLANG_VERSION" "$TC_DIR"
    rm -rf "$TC_DIR/.temp"
    echo -e "\nToolchain successfully downloaded and extracted!\n"
fi
export PATH="$TC_DIR/$CLANG_VERSION/bin:$PATH"

# If the -c flag is specified, perform a full clean
if [ "$CLEAN_BUILD" = true ]; then
    echo -e "Performing a full clean...\n"
    rm -rf out
fi

# Compilation Variables
export ARCH=arm64
export SUBARCH=arm64

# Apply defconfig
echo -e "Preparing kernel configuration...\n"

make O=out "../../arm64/configs/$DEFCONFIG"

# Start the build for Image.gz
echo -e "\nStarting kernel compilation...\n"

if make -j$(nproc --all) \
    O=out \
    CC="ccache clang" \
    AR=llvm-ar \
    NM=llvm-nm \
    LD=ld.lld \
    STRIP=llvm-strip \
    LLVM=1 \
    LLVM_IAS=1 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
    Image.gz; then

    echo -e "\nKernel compiled successfully! Packing into a zip archive...\n"

    # Cloning AnyKernel3
    git clone -q --depth=1 --branch=master https://github.com/pissarro-development/anykernel3 anykernel3

    # Copying the compiled images
    cp out/arch/arm64/boot/Image.gz anykernel3/

    # Creating the zip archive
    (cd anykernel3 && zip -r9 "../$ZIPNAME" ./* -x '*.git*' README.md '*placeholder')

    # Cleanup
    rm -rf anykernel3

    echo -e "\nCompleted in $((SECONDS / 60)) min(s) and $((SECONDS % 60)) sec(s)!"
    echo "Kernel installer zip: $ZIPNAME"
else
    echo -e "\nBuild failed!"
    exit 1
fi
