# Cross toolchain for building pivid-player inside the pivid-builder image.
# The image carries gcc-aarch64-linux-gnu and arm64 multiarch dev libs, and
# exports PKG_CONFIG_PATH/LIBDIR/SYSROOT_DIR pointing at the aarch64 pkgconfig
# dir -- so CMake's pkg_check_modules() resolves arm64 .pc files. Programs
# (pkg-config) must still be found on the host, hence PROGRAM mode NEVER.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)

# Multiarch sysroot is "/" (libs in /usr/lib/aarch64-linux-gnu, headers shared).
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
