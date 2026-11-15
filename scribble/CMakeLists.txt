cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(Scribble C CXX)

# Build with the Retro68 PowerPC toolchain: ./build.sh scribble
add_application(Scribble scribble.c scribble.r CONSOLE)
target_compile_options(Scribble PRIVATE -O2 -Wall)
# engine.h and hook.h, for the trap timing test (it reads Enforcer's state).
target_include_directories(Scribble PRIVATE ../common)
# MP tasks, for the Y test.
target_link_libraries(Scribble MPLibrary)

# The same program with a 1 MB size, which gets a sparse partition (the E
# test).
add_application(ScribbleBig scribble.c scribble-big.r CONSOLE)
target_compile_options(ScribbleBig PRIVATE -O2 -Wall)
target_include_directories(ScribbleBig PRIVATE ../common)
target_link_libraries(ScribbleBig MPLibrary)
