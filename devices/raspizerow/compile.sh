#!/bin/bash
# ADeus Raspberry Pi Zero Compile Script

set -e

ENABLE_BT=OFF

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --bluetooth|-b)
            ENABLE_BT=ON
            shift
            ;;
        --clean|-c)
            echo "Cleaning build directory..."
            rm -rf build
            shift
            ;;
        --help|-h)
            echo "Usage: ./compile.sh [options]"
            echo ""
            echo "Options:"
            echo "  --bluetooth, -b   Enable Bluetooth LE support"
            echo "  --clean, -c       Clean build directory first"
            echo "  --help, -h        Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Install dependencies if needed
if [ "$ENABLE_BT" = "ON" ]; then
    if ! dpkg -l | grep -q libbluetooth-dev; then
        echo "Installing Bluetooth development libraries..."
        sudo apt-get update
        sudo apt-get install -y libbluetooth-dev
    fi
fi

# Create build directory
mkdir -p build
cd build

# Generate build files using CMake
echo "Configuring with CMake (Bluetooth: $ENABLE_BT)..."
cmake -DENABLE_BLUETOOTH=$ENABLE_BT ..

# Build the project
echo "Building..."
make -j$(nproc)

# Copy to parent directory
cp -f main ..

echo ""
echo "Build complete! Run with: ./main --help"
