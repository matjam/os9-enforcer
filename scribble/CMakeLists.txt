cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(Scribble C CXX)

# Build with the Retro68 PowerPC toolchain:
#   cmake -S scribble -B scribble/build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
add_application(Scribble scribble.c CONSOLE)
target_compile_options(Scribble PRIVATE -O2 -Wall)
