cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(Probe C CXX)

# Build with the Retro68 PowerPC toolchain from os9-protect: ./build.sh probe
add_application(Probe probe.c CONSOLE)
target_compile_options(Probe PRIVATE -O2 -Wall)
