cmake_minimum_required(VERSION 3.15)
# Retro68's add_application links with the C++ driver.
project(Enforcer C CXX)

# Build with the Retro68 PowerPC toolchain: ./build.sh enforcer
# Produces "Enforcer Extension.bin" (a background-only application, file
# type 'appe', for the Extensions folder) and "Enforcer.bin" (the control
# panel, file type 'APPC', for the Control Panels folder).
set(COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../common)
include(${COMMON}/hooks.cmake)

# icon-extension.r and icon-panel.r are written by tools/make-icons.py.
add_application(EnforcerExt extension.c ${COMMON}/engine.c extension.r
                icon-extension.r TYPE appe CREATOR EnfX)
hooks_embed(EnforcerExt)
target_compile_options(EnforcerExt PRIVATE -O2 -Wall)

add_application(EnforcerPanel panel.c panel.r icon-panel.r
                TYPE APPC CREATOR Enfr)
target_include_directories(EnforcerPanel PRIVATE ${COMMON})
target_compile_options(EnforcerPanel PRIVATE -O2 -Wall)

# The Finder shows the name inside the MacBinary file, which Retro68 takes
# from the target. Repack each with the name users see, and set the
# hasBundle and hasCustomIcon flags so the Finder shows its icon.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(finder_name target name type creator)
    add_custom_command(
        OUTPUT "${name}.bin"
        COMMAND ${REZ} --copy ${target}.bin --data ${target}.pef
                -o "${name}.bin" -t ${type} -c ${creator}
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/finder-flags.py
                --bundle --custom-icon "${name}.bin"
        DEPENDS ${target}_APPL ${CMAKE_CURRENT_BINARY_DIR}/${target}.bin)
    add_custom_target(${target}_named ALL DEPENDS "${name}.bin")
endfunction()
finder_name(EnforcerExt "Enforcer Extension" appe EnfX)
finder_name(EnforcerPanel "Enforcer" APPC Enfr)
