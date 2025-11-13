#!/bin/bash
# Script to apply library patches for ESP32 PICO D4 project

echo "Applying library patches..."

# Fix 1: AsyncTCP.h - Add const to status() method
echo "Patching AsyncTCP.h..."
ASYNC_TCP_H=$(find .pio/libdeps/pico32/AsyncTCP* -name "AsyncTCP.h" 2>/dev/null | grep "src-" | head -1)
if [ -n "$ASYNC_TCP_H" ]; then
    if grep -q "uint8_t status();" "$ASYNC_TCP_H"; then
        sed -i 's/uint8_t status();/uint8_t status() const;/' "$ASYNC_TCP_H"
        echo "  ✓ AsyncTCP.h patched"
    else
        echo "  ✓ AsyncTCP.h already patched"
    fi
else
    echo "  ✗ AsyncTCP.h not found"
fi

# Fix 2: AsyncTCP.cpp - Add const to status() implementation
echo "Patching AsyncTCP.cpp..."
ASYNC_TCP_CPP=$(find .pio/libdeps/pico32/AsyncTCP* -name "AsyncTCP.cpp" 2>/dev/null | grep "src-" | head -1)
if [ -n "$ASYNC_TCP_CPP" ]; then
    if grep -q "uint8_t AsyncServer::status(){" "$ASYNC_TCP_CPP"; then
        sed -i 's/uint8_t AsyncServer::status(){/uint8_t AsyncServer::status() const {/' "$ASYNC_TCP_CPP"
        echo "  ✓ AsyncTCP.cpp patched"
    else
        echo "  ✓ AsyncTCP.cpp already patched"
    fi
else
    echo "  ✗ AsyncTCP.cpp not found"
fi

# Fix 3: FSWebServer.cpp - Add const to AsyncWebParameter pointers
echo "Patching FSWebServer.cpp..."
FSWEBSERVER_CPP=".pio/libdeps/pico32/SdWiFiBrowser/FSWebServer.cpp"
if [ -f "$FSWEBSERVER_CPP" ]; then
    if grep -q "AsyncWebParameter\* p = request->getParam" "$FSWEBSERVER_CPP"; then
        sed -i 's/AsyncWebParameter\* p = request->getParam/const AsyncWebParameter* p = request->getParam/g' "$FSWEBSERVER_CPP"
        echo "  ✓ FSWebServer.cpp patched"
    else
        echo "  ✓ FSWebServer.cpp already patched"
    fi
else
    echo "  ✗ FSWebServer.cpp not found"
fi

echo "Patches applied successfully!"
