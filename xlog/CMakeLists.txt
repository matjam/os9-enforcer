cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(XLog C CXX)

# Build with the Retro68 PowerPC toolchain:
#   cmake -S xlog -B xlog/build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
# XLog logs cross-application stores; Guard is the same program built to
# deny the ones made by an application's own code.
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/hooks.cmake)
set(ENGINE ${CMAKE_CURRENT_SOURCE_DIR}/../common/engine.c)
add_application(XLog xlog.c ${ENGINE} xlog.r CONSOLE)
hooks_embed(XLog)
target_compile_options(XLog PRIVATE -O2 -Wall)

add_application(Guard xlog.c ${ENGINE} xlog.r CONSOLE)
hooks_embed(Guard)
target_compile_options(Guard PRIVATE -O2 -Wall)
target_compile_definitions(Guard PRIVATE XLOG_ENFORCE=1)
