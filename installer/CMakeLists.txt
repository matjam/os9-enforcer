cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(EnforcerInstaller C CXX)

# Build with the Retro68 PowerPC toolchain from os9-protect: ./build.sh installer
# Produces "Enforcer Installer.bin". kpatch.h is written by
# tools/make-kpatch.py; the icons are the control panel's.
add_application(EnforcerInstaller installer.c rompatch.c lzss.c installer.r
                ../enforcer/icon-panel.r TYPE APPL CREATOR EnfI)
target_compile_options(EnforcerInstaller PRIVATE -O2 -Wall)

# Repack with the name users see, and set the hasBundle and hasCustomIcon
# flags so the Finder shows its icon (as ../enforcer/CMakeLists.txt).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT "Enforcer Installer.bin"
    COMMAND ${REZ} --copy EnforcerInstaller.bin --data EnforcerInstaller.pef
            -o "Enforcer Installer.bin" -t APPL -c EnfI
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/finder-flags.py
            --bundle --custom-icon "Enforcer Installer.bin"
    DEPENDS EnforcerInstaller_APPL
            ${CMAKE_CURRENT_BINARY_DIR}/EnforcerInstaller.bin)
add_custom_target(EnforcerInstaller_named ALL
                  DEPENDS "Enforcer Installer.bin")
