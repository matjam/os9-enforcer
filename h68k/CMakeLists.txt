cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(H68KTest C CXX)

# Build with the Retro68 PowerPC toolchain from os9-protect: ./build.sh h68k
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/hooks.cmake)
add_application(H68KTest h68ktest.c CONSOLE)
hooks_embed(H68KTest)
target_compile_options(H68KTest PRIVATE -O2 -Wall)
