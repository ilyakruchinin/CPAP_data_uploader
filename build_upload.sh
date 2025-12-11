#!/bin/bash
# Build and upload firmware to ESP32

set -e  # Exit on error

# Function to show usage
show_usage() {
    echo "Usage: $0 [build|upload|both] [port]"
    echo ""
    echo "Commands:"
    echo "  build         - Build firmware only (no sudo required)"
    echo "  upload        - Upload firmware only (requires sudo)"
    echo "  both          - Build and upload (default, requires sudo for upload)"
    echo ""
    echo "Options:"
    echo "  port          - Serial port (e.g., /dev/ttyUSB0, /dev/ttyACM0)"
    echo "                  If not specified, PlatformIO will auto-detect"
    echo ""
    echo "Examples:"
    echo "  $0 build                    # Build only"
    echo "  $0 upload                   # Upload only (requires previous build)"
    echo "  $0 upload /dev/ttyUSB0      # Upload to specific port"
    echo "  $0 both                     # Build and upload"
    echo "  $0                          # Same as 'both'"
}

# Parse command line arguments
COMMAND=${1:-both}
PORT=$2

# Validate command
case $COMMAND in
    build|upload|both)
        ;;
    -h|--help|help)
        show_usage
        exit 0
        ;;
    *)
        echo "Error: Invalid command '$COMMAND'"
        echo ""
        show_usage
        exit 1
        ;;
esac

# Check if venv exists
if [ ! -d "venv" ]; then
    echo "Error: Virtual environment not found."
    echo "Please run ./setup.sh first to set up the development environment."
    exit 1
fi

# Activate virtual environment
source venv/bin/activate

# Check if pio is available
if ! command -v pio &> /dev/null; then
    echo "Error: PlatformIO not found in virtual environment."
    echo "Please run ./setup.sh to install dependencies."
    exit 1
fi

# Get PlatformIO path for sudo usage
PIO_PATH=$(which pio)

# Build step
if [ "$COMMAND" = "build" ] || [ "$COMMAND" = "both" ]; then
    echo "Building firmware..."
    pio run -e pico32
    echo "✅ Build completed successfully!"
    echo "Firmware location: .pio/build/pico32/firmware.bin"
fi

# Upload step
if [ "$COMMAND" = "upload" ] || [ "$COMMAND" = "both" ]; then
    echo "Uploading firmware..."
    
    # Check if firmware exists
    if [ ! -f ".pio/build/pico32/firmware.bin" ]; then
        echo "Error: Firmware not found. Please build first with:"
        echo "  $0 build"
        exit 1
    fi
    
    # Prepare upload command
    UPLOAD_CMD="$PIO_PATH run -e pico32 -t upload"
    
    # Add port if specified
    if [ -n "$PORT" ]; then
        UPLOAD_CMD="$UPLOAD_CMD --upload-port $PORT"
        echo "Using port: $PORT"
    else
        echo "Auto-detecting serial port..."
    fi
    
    # Upload with sudo (required for serial port access)
    echo "Note: sudo required for serial port access"
    sudo -E $UPLOAD_CMD
    echo "✅ Upload completed successfully!"
fi

echo ""
echo "Done! 🚀"
