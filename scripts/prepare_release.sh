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
BUILD_DIR_OTA=".pio/build/pico32-ota"
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

echo -e "${GREEN}Preparing release package...${NC}"

# Generate version header
echo -e "${YELLOW}Generating version information...${NC}"
python3 scripts/generate_version.py include/version.h

# Build firmware
echo -e "${YELLOW}Building firmware...${NC}"

# Check if virtual environment exists and activate it
if [ -d "venv" ]; then
    source venv/bin/activate
    echo "Activated virtual environment"
fi

echo "Building OTA firmware (pico32-ota)..."

# Hybrid compile overwrites sdkconfig.defaults with a large generated config.
# Back it up and restore after build to keep the repo clean.
SDKCONFIG_DEFAULTS="sdkconfig.defaults"
SDKCONFIG_BACKUP="${SDKCONFIG_DEFAULTS}.bak"
if [ -f "$SDKCONFIG_DEFAULTS" ]; then
    cp "$SDKCONFIG_DEFAULTS" "$SDKCONFIG_BACKUP"
fi

build_ok=true
if ! pio run -e pico32-ota; then
    echo -e "${RED}Failed to build OTA firmware${NC}"
    build_ok=false
fi

# Restore sdkconfig.defaults regardless of build outcome
if [ -f "$SDKCONFIG_BACKUP" ]; then
    mv "$SDKCONFIG_BACKUP" "$SDKCONFIG_DEFAULTS"
    echo "Restored $SDKCONFIG_DEFAULTS"
fi

if [ "$build_ok" = false ]; then
    exit 1
fi

if [ ! -f "$FIRMWARE_BIN_OTA" ]; then
    echo -e "${RED}OTA firmware not found: $FIRMWARE_BIN_OTA${NC}"
    exit 1
fi

# Check if esptool.exe exists for Windows package
# Note: No longer needed as we use PlatformIO for Windows uploads

# Create temporary release directory
TEMP_DIR="$RELEASE_DIR/$RELEASE_NAME"
mkdir -p "$TEMP_DIR"

# Copy firmware files with descriptive names
echo "Copying firmware files..."

# Create merged binary for flashing
echo "Creating merged binary..."
python -m esptool --chip esp32 merge-bin \
    -o "$TEMP_DIR/firmware-ota.bin" \
    --flash-mode dio --flash-freq 40m --flash-size 4MB \
    0x1000 "$BUILD_DIR_OTA/bootloader.bin" \
    0x8000 "$BUILD_DIR_OTA/partitions.bin" \
    0x10000 "$FIRMWARE_BIN_OTA"

# Copy app-only OTA upgrade binary
cp "$FIRMWARE_BIN_OTA" "$TEMP_DIR/firmware-ota-upgrade.bin"

# Keep copies in release folder for GitHub releases
echo "Keeping firmware copies in release folder..."
cp "$TEMP_DIR/firmware-ota.bin" "$RELEASE_DIR/firmware-ota-${VERSION}.bin"
cp "$TEMP_DIR/firmware-ota-upgrade.bin" "$RELEASE_DIR/firmware-ota-upgrade-${VERSION}.bin"

# Copy upload scripts
echo "Copying upload tools..."
cp "$RELEASE_DIR/upload.sh" "$TEMP_DIR/"
cp "$RELEASE_DIR/upload-ota.bat" "$TEMP_DIR/"
cp "$RELEASE_DIR/README.md" "$TEMP_DIR/"
cp "$RELEASE_DIR/requirements.txt" "$TEMP_DIR/"

# Copy config examples
echo "Copying config.txt example variants..."
cp docs/config.txt.example* "$TEMP_DIR/"

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
echo "  - firmware-ota-upgrade-${VERSION}.bin (app-only for OTA web updates)"
echo ""
echo "Package contents:"
echo "  - firmware-ota.bin (complete OTA firmware for initial flashing)"
echo "  - firmware-ota-upgrade.bin (app-only for OTA web updates)"
echo "  - upload.sh (macOS/Linux upload script)"
echo "  - upload-ota.bat (Windows upload script)"
echo "  - README.md (usage instructions)"
echo "  - requirements.txt (Python dependencies)"
echo "  - config.txt.example* (configuration templates: SMB, SleepHQ, combined)"
echo ""
echo -e "${GREEN}Firmware sizes:${NC}"
echo "  OTA merged:       $(du -h "$RELEASE_DIR/firmware-ota-${VERSION}.bin" | cut -f1) (complete, for initial flash)"
echo "  OTA upgrade:      $(du -h "$RELEASE_DIR/firmware-ota-upgrade-${VERSION}.bin" | cut -f1) (app-only, for web updates)"
echo ""
echo -e "${YELLOW}Next steps for GitHub release:${NC}"
echo "1. Upload the release package: $RELEASE_DIR/$RELEASE_PACKAGE"
echo "2. Upload individual firmware files from release/ folder:"
echo "   - firmware-ota-${VERSION}.bin (for initial flashing)" 
echo "   - firmware-ota-upgrade-${VERSION}.bin (for OTA web updates)"
