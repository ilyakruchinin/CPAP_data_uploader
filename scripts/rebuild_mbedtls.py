"""
PlatformIO extra_script: Rebuild mbedTLS TLS library with custom configuration.

The stock Arduino-ESP32 framework ships pre-compiled mbedTLS .a files that ignore
sdkconfig.defaults for buffer sizes, asymmetric content length, and feature toggles.
This script downloads the matching mbedTLS 2.28.7 source, cross-compiles the TLS
library objects with our custom defines, and links them BEFORE the framework library
so our symbols take precedence.

Key changes enabled:
  - Asymmetric TLS buffers: 16KB IN / 4KB OUT (saves 12KB per connection)
  - Variable buffer length: shrink buffers after handshake
  - Disabled session tickets, renegotiation, DTLS features
  - Disabled peer certificate retention after verification

Usage: Added as extra_script in platformio.ini:
  extra_scripts = pre:scripts/rebuild_mbedtls.py
"""

Import("env")
import os
import sys
import hashlib
import tarfile
import shutil
import subprocess

MBEDTLS_VERSION = "2.28.7"
MBEDTLS_URL = f"https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v{MBEDTLS_VERSION}.tar.gz"
MBEDTLS_SHA256 = None  # Set after first successful download if you want integrity checking

# Where we cache the download and build artifacts
BUILD_DIR = os.path.join(env.subst("$BUILD_DIR"), "mbedtls_custom")
CACHE_DIR = os.path.join(env.subst("$PROJECT_DIR"), ".pio", "mbedtls_cache")
TARBALL = os.path.join(CACHE_DIR, f"mbedtls-{MBEDTLS_VERSION}.tar.gz")
SRC_DIR = os.path.join(CACHE_DIR, f"mbedtls-{MBEDTLS_VERSION}", "library")
MARKER = os.path.join(BUILD_DIR, f"libmbedtls_2_custom.built")

# Framework paths
FRAMEWORK_DIR = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
FRAMEWORK_SDK = os.path.join(FRAMEWORK_DIR, "tools", "sdk", "esp32")
FRAMEWORK_LIB = os.path.join(FRAMEWORK_SDK, "lib")
ORIG_LIB = os.path.join(FRAMEWORK_LIB, "libmbedtls_2.a")

# Include paths for compilation
MBEDTLS_INC = os.path.join(FRAMEWORK_SDK, "include", "mbedtls", "mbedtls", "include")
MBEDTLS_PORT_INC = os.path.join(FRAMEWORK_SDK, "include", "mbedtls", "port", "include")
ESP_IDF_INC = os.path.join(FRAMEWORK_SDK, "include", "esp_idf_version")

# Source files to RECOMPILE with our custom defines.
# These contain buffer allocation, feature toggles, and protocol logic
# affected by our config changes (asymmetric buffers, disabled features).
TLS_SOURCES = [
    "ssl_cache.c",
    "ssl_ciphersuites.c",
    "ssl_cli.c",
    "ssl_cookie.c",
    "ssl_msg.c",
    "ssl_srv.c",
    "ssl_ticket.c",
    "ssl_tls.c",
    "ssl_tls13_keys.c",
]

# Objects to KEEP from the original framework archive.
# These are ESP-IDF custom wrappers that differ from upstream mbedTLS:
#   - net_sockets.c: ESP-IDF provides mbedtls_net_recv/send (upstream is empty
#     because esp_config.h undefines MBEDTLS_NET_C)
#   - debug.c: upstream debug, kept as-is (no custom defines needed)
#   - mbedtls_debug.c: ESP-IDF debug wrapper
PRESERVED_OBJECTS = [
    "debug.c.obj",
    "net_sockets.c.obj",
    "mbedtls_debug.c.obj",
]

# Custom defines that override the framework defaults.
# These map to CONFIG_MBEDTLS_* settings in sdkconfig.defaults but need to be
# applied directly since we're compiling outside the normal IDF build system.
CUSTOM_DEFINES = [
    # 6.1: Asymmetric buffers (16KB IN, 4KB OUT)
    "MBEDTLS_SSL_IN_CONTENT_LEN=16384",
    "MBEDTLS_SSL_OUT_CONTENT_LEN=4096",
    # Variable buffer length — allows shrinking after handshake
    "MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH",
    # Don't keep peer cert after verification (saves 2-4KB)
    # (undefine MBEDTLS_SSL_KEEP_PEER_CERTIFICATE — handled via undef below)

    # 6.2: Disable unused TLS features
    # (undefine via -U flags below)
]

# Defines to UNDEFINE (features to disable)
CUSTOM_UNDEFINES = [
    "MBEDTLS_SSL_KEEP_PEER_CERTIFICATE",
    "MBEDTLS_SSL_SESSION_TICKETS",
    "MBEDTLS_SSL_RENEGOTIATION",
    "MBEDTLS_SSL_DTLS_HELLO_VERIFY",
    "MBEDTLS_SSL_DTLS_CONNECTION_ID",
]


def download_mbedtls():
    """Download mbedTLS source tarball if not cached."""
    if os.path.exists(TARBALL):
        return True

    os.makedirs(CACHE_DIR, exist_ok=True)
    print(f"  Downloading mbedTLS {MBEDTLS_VERSION}...")

    try:
        # Use Python's urllib to avoid external dependencies
        import urllib.request
        urllib.request.urlretrieve(MBEDTLS_URL, TARBALL)
        return True
    except Exception as e:
        print(f"  ERROR: Failed to download mbedTLS: {e}")
        print(f"  URL: {MBEDTLS_URL}")
        print(f"  You can manually download and place at: {TARBALL}")
        return False


def extract_mbedtls():
    """Extract the library/ directory from the tarball."""
    if os.path.exists(SRC_DIR):
        return True

    if not os.path.exists(TARBALL):
        return False

    print(f"  Extracting mbedTLS {MBEDTLS_VERSION} library sources...")
    try:
        with tarfile.open(TARBALL, "r:gz") as tar:
            # Extract only the library/ directory
            members = [m for m in tar.getmembers()
                       if m.name.startswith(f"mbedtls-{MBEDTLS_VERSION}/library/")]
            tar.extractall(CACHE_DIR, members=members)
        return True
    except Exception as e:
        print(f"  ERROR: Failed to extract mbedTLS: {e}")
        return False


def get_compiler():
    """Get the xtensa cross-compiler path."""
    cc = env.subst("$CC")
    if cc and os.path.exists(cc):
        return cc
    # Fallback: search in toolchain package
    toolchain = env.PioPlatform().get_package_dir("toolchain-xtensa-esp32")
    return os.path.join(toolchain, "bin", "xtensa-esp32-elf-gcc")


def get_archiver():
    """Get the xtensa archiver path."""
    ar = env.subst("$AR")
    if ar and os.path.exists(ar):
        return ar
    toolchain = env.PioPlatform().get_package_dir("toolchain-xtensa-esp32")
    return os.path.join(toolchain, "bin", "xtensa-esp32-elf-ar")


def find_sdkconfig_h():
    """Find the sdkconfig.h matching the board's flash mode variant."""
    # The Arduino-ESP32 framework stores variant-specific sdkconfig.h files
    # in tools/sdk/esp32/<flash_mode>_<flash_freq>/include/sdkconfig.h
    # Common variants: dio_qspi, qio_qspi, dout_qspi
    variant_dirs = [
        "dio_qspi",   # Most common (pico32, esp32dev)
        "qio_qspi",
        "dout_qspi",
        "qout_qspi",
    ]
    for variant in variant_dirs:
        candidate = os.path.join(FRAMEWORK_SDK, variant, "include", "sdkconfig.h")
        if os.path.exists(candidate):
            return candidate

    # Fallback: search all variant directories
    for entry in os.listdir(FRAMEWORK_SDK):
        candidate = os.path.join(FRAMEWORK_SDK, entry, "include", "sdkconfig.h")
        if os.path.exists(candidate):
            return candidate
    return None


def compile_mbedtls():
    """Cross-compile mbedTLS TLS library objects with custom config."""
    os.makedirs(BUILD_DIR, exist_ok=True)

    cc = get_compiler()
    ar = get_archiver()

    if not os.path.exists(cc):
        print(f"  ERROR: Compiler not found: {cc}")
        return False

    # Find sdkconfig.h
    sdkconfig_h = find_sdkconfig_h()

    # Build include paths
    include_paths = [
        MBEDTLS_INC,
        MBEDTLS_PORT_INC,
        SRC_DIR,  # For library-internal headers (common.h, etc.)
    ]

    # Add all ESP-IDF include directories that the framework uses
    esp_include_base = os.path.join(FRAMEWORK_SDK, "include")
    for subdir in ["mbedtls/mbedtls/include", "mbedtls/port/include",
                   "mbedtls/esp_crt_bundle/include",
                   "esp_common/include", "esp_system/include",
                   "newlib/platform_include", "freertos/include",
                   "freertos/include/esp_additions",
                   "freertos/port/xtensa/include",
                   "esp_hw_support/include", "xtensa/include",
                   "xtensa/esp32/include", "esp_rom/include",
                   "soc/esp32/include", "soc/include",
                   "hal/include", "hal/esp32/include",
                   "esp_timer/include", "log/include",
                   "heap/include"]:
        full = os.path.join(esp_include_base, subdir)
        if os.path.exists(full):
            include_paths.append(full)

    # Add config directory if sdkconfig.h was found
    if sdkconfig_h:
        include_paths.append(os.path.dirname(sdkconfig_h))

    # Compiler flags matching the framework build
    cflags = [
        "-mlongcalls",
        "-Os",
        "-ffunction-sections",
        "-fdata-sections",
        "-fstrict-volatile-bitfields",
        "-DESP_PLATFORM",
        "-DESP32",
        "-DMBEDTLS_CONFIG_FILE=\"mbedtls/esp_config.h\"",
        "-DHAVE_CONFIG_H",
    ]

    # Add our custom defines
    for d in CUSTOM_DEFINES:
        cflags.append(f"-D{d}")

    # Add our undefines (applied AFTER includes via a wrapper technique)
    # Since -U only works if the symbol was previously defined via -D,
    # and esp_config.h defines these via #ifdef CONFIG_*, we use a
    # different approach: define the CONFIG_ variables to 0/undef them
    for u in CUSTOM_UNDEFINES:
        # Map MBEDTLS_ names back to CONFIG_MBEDTLS_ names
        config_name = u.replace("MBEDTLS_", "CONFIG_MBEDTLS_", 1)
        cflags.append(f"-U{config_name}")

    # Add include paths
    for inc in include_paths:
        cflags.append(f"-I{inc}")

    # Compile each source file
    objects = []
    for src_name in TLS_SOURCES:
        src_path = os.path.join(SRC_DIR, src_name)
        if not os.path.exists(src_path):
            # Some files might have ESP-IDF specific wrappers
            esp_src = os.path.join(SRC_DIR, f"esp_{src_name}")
            if os.path.exists(esp_src):
                src_path = esp_src
            else:
                print(f"  WARNING: Source not found, skipping: {src_name}")
                continue

        obj_path = os.path.join(BUILD_DIR, f"{src_name}.obj")
        objects.append(obj_path)

        # Skip if object is newer than source
        if os.path.exists(obj_path) and os.path.getmtime(obj_path) > os.path.getmtime(src_path):
            continue

        cmd = [cc] + cflags + ["-c", src_path, "-o", obj_path]
        print(f"  CC {src_name}")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  ERROR compiling {src_name}:")
            # Show just the first few error lines
            for line in result.stderr.split("\n")[:15]:
                print(f"    {line}")
            return False

    if not objects:
        print("  ERROR: No objects compiled")
        return False

    # Extract preserved objects from the original (or backup) framework archive
    backup = ORIG_LIB + ".orig"
    orig_archive = backup if os.path.exists(backup) else ORIG_LIB
    extract_dir = os.path.join(BUILD_DIR, "orig_objs")
    os.makedirs(extract_dir, exist_ok=True)

    for obj_name in PRESERVED_OBJECTS:
        cmd = [ar, "x", orig_archive, obj_name]
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=extract_dir)
        if result.returncode == 0 and os.path.exists(os.path.join(extract_dir, obj_name)):
            objects.append(os.path.join(extract_dir, obj_name))
            print(f"  Preserved {obj_name} from original archive")
        else:
            print(f"  WARNING: Could not extract {obj_name} from original archive")

    # Create the static library
    lib_path = os.path.join(BUILD_DIR, "libmbedtls_2_custom.a")
    cmd = [ar, "rcs", lib_path] + objects
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  ERROR creating archive: {result.stderr}")
        return False

    print(f"  Created {lib_path} ({len(objects)} objects)")

    # Write marker file
    with open(MARKER, "w") as f:
        f.write(f"mbedtls-{MBEDTLS_VERSION}\n")

    return True


def install_custom_lib():
    """Replace the framework's libmbedtls_2.a with our custom build.
    
    The ESP32 Arduino linker script hardcodes -lmbedtls_2 and ignores
    PlatformIO LIBS additions, so we must replace the .a in the framework
    directory.  A .orig backup is kept for restoration.
    """
    custom_lib = os.path.join(BUILD_DIR, "libmbedtls_2_custom.a")
    if not os.path.exists(custom_lib):
        return

    backup = ORIG_LIB + ".orig"

    # Back up the original if not already done
    if not os.path.exists(backup):
        shutil.copy2(ORIG_LIB, backup)
        print(f"  Backed up original: {os.path.basename(backup)}")

    # Replace with our custom build
    shutil.copy2(custom_lib, ORIG_LIB)
    print(f"  Installed custom libmbedtls_2.a (asymmetric 16KB/4KB)")


def build_custom_mbedtls(source, target, env):
    """Main build function called as pre-build action."""
    # Check if already built
    if os.path.exists(MARKER):
        custom_lib = os.path.join(BUILD_DIR, "libmbedtls_2_custom.a")
        if os.path.exists(custom_lib):
            install_custom_lib()  # Ensure installed even on incremental builds
            return

    print("=" * 60)
    print(f"Building custom mbedTLS {MBEDTLS_VERSION} with asymmetric buffers")
    print("  IN=16KB, OUT=4KB (saves 12KB per TLS connection)")
    print("=" * 60)

    if not download_mbedtls():
        print("  FALLBACK: Using stock mbedTLS (symmetric 16KB buffers)")
        return

    if not extract_mbedtls():
        print("  FALLBACK: Using stock mbedTLS (symmetric 16KB buffers)")
        return

    if not compile_mbedtls():
        print("  FALLBACK: Using stock mbedTLS (symmetric 16KB buffers)")
        # Clean up failed build
        if os.path.exists(MARKER):
            os.remove(MARKER)
        return

    print("  Custom mbedTLS build complete!")
    print("=" * 60)

    # Install into framework directory so the hardcoded -lmbedtls_2 picks it up
    install_custom_lib()


# Run the build immediately during script loading (pre: script phase)
# This ensures the custom library exists before any compilation starts.
build_custom_mbedtls(None, None, env)
