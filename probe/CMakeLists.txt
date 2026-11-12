cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(Probe C CXX)

# Build with the Retro68 PowerPC toolchain:
#   cmake -S probe -B probe/build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
add_application(Probe probe.c CONSOLE)
target_compile_options(Probe PRIVATE -O2 -Wall)
