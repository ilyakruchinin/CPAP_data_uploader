Import("env")
import os

# Get the library source directory from the environment
lib_source_dir = env.get("PROJECT_SRC_DIR", "").replace("/src", "/components/libsmb2")

# Add include paths relative to library root
env.Append(CPPPATH=[
    os.path.join(lib_source_dir, "include"),
    os.path.join(lib_source_dir, "include/smb2"),
    os.path.join(lib_source_dir, "include/esp"),
    os.path.join(lib_source_dir, "lib"),
])

# Add compile definitions
env.Append(CPPDEFINES=[
    "HAVE_CONFIG_H",
    "NEED_READV",
    "NEED_WRITEV",
    "NEED_GETLOGIN_R",
    "NEED_RANDOM",
    "NEED_SRANDOM",
    ("_U_", ""),
])

# Add C flags
env.Append(CCFLAGS=[
    "-Wno-implicit-function-declaration",
    "-Wno-builtin-declaration-mismatch",
    "-include", os.path.join(lib_source_dir, "lib/esp_compat_wrapper.h"),
])
