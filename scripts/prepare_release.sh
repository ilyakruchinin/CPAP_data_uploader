#!/bin/bash
# Script to prepare a release package with precompiled firmware

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Configuration
RELEASE_DIR="release"
BUILD_DIR=".pio/build/pico32"
FIRMWARE_BIN="$BUILD_DIR/firmware.bin"

# Get version from git tag (latest tag on current commit)
GIT_TAG=$(git describe --tags --exact-match 2>/dev/null || echo "")
if [ -z "$GIT_TAG" ]; then
    # If no tag on current commit, use latest tag with commit count
    GIT_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")
    if [ -z "$GIT_TAG" ]; then
        # No tags at all, use date
        VERSION=$(date +%Y%m%d-%H%M%S)
        echo -e "${YELLOW}Warning: No git tags found. Using date-based version: $VERSION${NC}"
    else
        COMMIT_COUNT=$(git rev-list ${GIT_TAG}..HEAD --count)
        if [ "$COMMIT_COUNT" -gt 0 ]; then
            VERSION="${GIT_TAG}-dev+${COMMIT_COUNT}"
            echo -e "${YELLOW}Warning: Current commit is not tagged. Using: $VERSION${NC}"
        else
            VERSION="$GIT_TAG"
        fi
    fi
else
    VERSION="$GIT_TAG"
    echo -e "${GREEN}Using git tag version: $VERSION${NC}"
fi

RELEASE_NAME="cpap_uploader_${VERSION}"
RELEASE_PACKAGE="$RELEASE_NAME.zip"
ESPTOOL_WIN="$RELEASE_DIR/esptool.exe"

echo -e "${GREEN}Preparing release package...${NC}"

# Check if firmware exists
if [ ! -f "$FIRMWARE_BIN" ]; then
    echo -e "${YELLOW}Firmware not found. Building...${NC}"
    source venv/bin/activate
    pio run -e pico32
fi

# Check if esptool.exe exists for Windows package
if [ ! -f "$ESPTOOL_WIN" ]; then
    echo -e "${RED}Warning: esptool.exe not found at $ESPTOOL_WIN${NC}"
    echo "Please download esptool.exe and place it in the release/ directory"
    echo "Download from: https://github.com/espressif/esptool/releases"
    echo ""
    read -p "Continue without esptool.exe? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Create temporary release directory
TEMP_DIR="$RELEASE_DIR/$RELEASE_NAME"
mkdir -p "$TEMP_DIR"

# Copy firmware
echo "Copying firmware..."
cp "$FIRMWARE_BIN" "$TEMP_DIR/"

# Copy upload scripts
echo "Copying upload tools..."
cp "$RELEASE_DIR/upload.sh" "$TEMP_DIR/"
cp "$RELEASE_DIR/upload.bat" "$TEMP_DIR/"
cp "$RELEASE_DIR/README.md" "$TEMP_DIR/"
cp "$RELEASE_DIR/requirements.txt" "$TEMP_DIR/"

# Copy config example
echo "Copying config.json.example..."
cp "docs/config.json.example" "$TEMP_DIR/"

# Copy esptool.exe if it exists
if [ -f "$ESPTOOL_WIN" ]; then
    echo "Copying esptool.exe for Windows..."
    cp "$ESPTOOL_WIN" "$TEMP_DIR/"
fi

# Make scripts executable
chmod +x "$TEMP_DIR/upload.sh"

# Create zip package
echo "Creating release package..."
cd "$RELEASE_DIR"
zip -r "$RELEASE_PACKAGE" "$RELEASE_NAME"
cd ..

# Cleanup
rm -rf "$TEMP_DIR"

echo -e "${GREEN}Release package created: $RELEASE_DIR/$RELEASE_PACKAGE${NC}"
echo ""
echo "Package contents:"
echo "  - firmware.bin (precompiled firmware)"
echo "  - upload.sh (macOS/Linux upload script)"
echo "  - upload.bat (Windows upload script)"
if [ -f "$ESPTOOL_WIN" ]; then
    echo "  - esptool.exe (Windows upload tool)"
fi
echo "  - README.md (usage instructions)"
echo "  - requirements.txt (Python dependencies for macOS/Linux)"
echo "  - config.json.example (configuration template)"
