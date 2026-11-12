cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(WLog C CXX)

# Build with the Retro68 PowerPC toolchain:
#   cmake -S wlog -B wlog/build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/hooks.cmake)
add_application(WLog wlog.c CONSOLE)
hooks_embed(WLog)
target_compile_options(WLog PRIVATE -O2 -Wall)
