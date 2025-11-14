#!/bin/bash
# Setup script for libsmb2 integration
# This script clones and configures libsmb2 for ESP32 Arduino

set -e  # Exit on error

echo "=== libsmb2 Setup Script ==="
echo ""

# Check if we're in the project root
if [ ! -f "platformio.ini" ]; then
    echo "Error: platformio.ini not found. Please run this script from the project root."
    exit 1
fi

# Create components directory if it doesn't exist
if [ ! -d "components" ]; then
    echo "Creating components directory..."
    mkdir -p components
fi

# Clone libsmb2 if not already present
if [ -d "components/libsmb2" ]; then
    echo "libsmb2 already exists in components/"
    read -p "Do you want to update it? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Updating libsmb2..."
        cd components/libsmb2
        git pull
        cd ../..
    fi
else
    echo "Cloning libsmb2..."
    git clone https://github.com/sahlberg/libsmb2.git components/libsmb2
fi

# Copy integration files
echo ""
echo "Installing integration files..."

integration_source="scripts/libsmb2_integration_files"

if [ ! -d "$integration_source" ]; then
    echo "Error: Integration files not found at $integration_source"
    exit 1
fi

# Copy library.json
cp "$integration_source/library.json" components/libsmb2/
echo "  ✓ Copied library.json"

# Copy library_build.py
cp "$integration_source/library_build.py" components/libsmb2/
echo "  ✓ Copied library_build.py"

# Create lib directory if needed and copy esp_compat_wrapper.h
mkdir -p components/libsmb2/lib
cp "$integration_source/lib/esp_compat_wrapper.h" components/libsmb2/lib/
echo "  ✓ Copied esp_compat_wrapper.h"

# Check platformio.ini configuration
echo ""
echo "Checking platformio.ini configuration..."

if grep -q "lib_extra_dirs = components" platformio.ini; then
    echo "  ✓ lib_extra_dirs configured"
else
    echo "  ✗ lib_extra_dirs not found in platformio.ini"
    echo "    Add 'lib_extra_dirs = components' to your environment configuration"
    exit 1
fi

if grep -q "ENABLE_SMB_UPLOAD" platformio.ini; then
    echo "  ✓ ENABLE_SMB_UPLOAD flag found"
else
    echo "  ⚠ ENABLE_SMB_UPLOAD flag not found (optional)"
fi

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "  1. Build the project: pio run"
echo "  2. Upload to device: pio run --target upload"
echo "  3. Monitor serial: pio device monitor"
echo ""
echo "For more information, see docs/LIBSMB2_INTEGRATION.md"
