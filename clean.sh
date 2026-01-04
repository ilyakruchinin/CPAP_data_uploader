#!/bin/bash
# Clean build artifacts and temporary files

set -e  # Exit on error

echo "Cleaning CPAP Data Uploader project..."

# Remove build artifacts
if [ -d ".pio/build" ]; then
    echo "Removing build artifacts..."
    rm -rf .pio/build
fi

# Remove library dependencies (will be re-downloaded on next build)
if [ -d ".pio/libdeps" ]; then
    echo "Removing library dependencies..."
    rm -rf .pio/libdeps
fi

# Remove Python virtual environment
if [ -d "venv" ]; then
    echo "Removing Python virtual environment..."
    rm -rf venv
fi

# Remove libsmb2 component (will be re-cloned by setup script)
if [ -d "components/libsmb2" ]; then
    echo "Removing libsmb2 component..."
    rm -rf components/libsmb2
fi

# Remove any temporary files
echo "Removing temporary files..."
find . -name "*.tmp" -delete 2>/dev/null || true
find . -name "*.log" -delete 2>/dev/null || true
find . -name ".DS_Store" -delete 2>/dev/null || true

# Remove firmware binary if it exists in root
if [ -f "firmware.bin" ]; then
    echo "Removing firmware binary..."
    rm firmware.bin
fi

echo "✅ Cleanup completed!"
echo ""
echo "To set up the environment again, run:"
echo "  ./setup.sh"
echo ""
echo "To build firmware, run:"
echo "  ./build_upload.sh build [pico32|pico32-ota]"