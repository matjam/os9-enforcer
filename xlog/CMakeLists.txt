cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(XLog C CXX)

# Build with the Retro68 PowerPC toolchain:
#   cmake -S xlog -B xlog/build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/h68k.cmake)
add_application(XLog xlog.c xlog.r CONSOLE)
h68k_embed(XLog)
target_compile_options(XLog PRIVATE -O2 -Wall)
