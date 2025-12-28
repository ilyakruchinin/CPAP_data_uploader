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
BUILD_DIR_STANDARD=".pio/build/pico32"
BUILD_DIR_OTA=".pio/build/pico32-ota"
FIRMWARE_BIN_STANDARD="$BUILD_DIR_STANDARD/firmware.bin"
FIRMWARE_BIN_OTA="$BUILD_DIR_OTA/firmware.bin"

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

# Generate version header
echo -e "${YELLOW}Generating version information...${NC}"
python3 scripts/generate_version.py include/version.h

# Build both firmware targets
echo -e "${YELLOW}Building firmware targets...${NC}"

# Check if virtual environment exists and activate it
if [ -d "venv" ]; then
    source venv/bin/activate
    echo "Activated virtual environment"
fi

# Build standard firmware (no OTA)
echo "Building standard firmware (pico32)..."
if ! pio run -e pico32; then
    echo -e "${RED}Failed to build standard firmware${NC}"
    exit 1
fi

# Build OTA firmware
echo "Building OTA firmware (pico32-ota)..."
if ! pio run -e pico32-ota; then
    echo -e "${RED}Failed to build OTA firmware${NC}"
    exit 1
fi

# Check if firmware files exist
if [ ! -f "$FIRMWARE_BIN_STANDARD" ]; then
    echo -e "${RED}Standard firmware not found: $FIRMWARE_BIN_STANDARD${NC}"
    exit 1
fi

if [ ! -f "$FIRMWARE_BIN_OTA" ]; then
    echo -e "${RED}OTA firmware not found: $FIRMWARE_BIN_OTA${NC}"
    exit 1
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

# Copy firmware files with descriptive names
echo "Copying firmware files..."
cp "$FIRMWARE_BIN_OTA" "$TEMP_DIR/firmware-ota.bin"
cp "$FIRMWARE_BIN_STANDARD" "$TEMP_DIR/firmware-standard.bin"

# Keep copies in release folder for GitHub releases
echo "Keeping firmware copies in release folder..."
cp "$FIRMWARE_BIN_OTA" "$RELEASE_DIR/firmware-ota-${VERSION}.bin"
cp "$FIRMWARE_BIN_STANDARD" "$RELEASE_DIR/firmware-standard-${VERSION}.bin"

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
echo -e "${GREEN}Firmware copies saved in release folder:${NC}"
echo "  - firmware-ota-${VERSION}.bin (OTA-enabled with web updates)"
echo "  - firmware-standard-${VERSION}.bin (standard firmware, 3MB app space)"
echo ""
echo "Package contents:"
echo "  - firmware-ota.bin (OTA-enabled firmware with web updates)"
echo "  - firmware-standard.bin (standard firmware, 3MB app space)"
echo "  - upload.sh (macOS/Linux upload script)"
echo "  - upload.bat (Windows upload script)"
if [ -f "$ESPTOOL_WIN" ]; then
    echo "  - esptool.exe (Windows upload tool)"
fi
echo "  - README.md (usage instructions)"
echo "  - requirements.txt (Python dependencies for macOS/Linux)"
echo "  - config.json.example (configuration template)"
echo ""
echo -e "${GREEN}Firmware sizes:${NC}"
echo "  Standard: $(du -h "$FIRMWARE_BIN_STANDARD" | cut -f1) (3MB partition)"
echo "  OTA:      $(du -h "$FIRMWARE_BIN_OTA" | cut -f1) (1.5MB partition)"
echo ""
echo -e "${YELLOW}Next steps for GitHub release:${NC}"
echo "1. Upload the release package: $RELEASE_DIR/$RELEASE_PACKAGE"
echo "2. Upload individual firmware files from release/ folder:"
echo "   - firmware-ota-${VERSION}.bin" 
echo "   - firmware-standard-${VERSION}.bin"
