cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(TMem C CXX)

# Build with the Retro68 PowerPC toolchain from os9-protect: ./build.sh tmem
add_application(TMem tmem.c CONSOLE)
target_compile_options(TMem PRIVATE -O2 -Wall)
# kcb.h, for kcb_physical.
target_include_directories(TMem PRIVATE ../common)
