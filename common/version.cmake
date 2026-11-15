# The version of this build (tools/make-vers.py): version.r, with the
# 'vers' resources the Finder shows in Get Info, and version.h, which
# defines ENFORCER_VERSION. Written into the build folder each time CMake
# configures, which build.sh does on every run. Add ${VERSION_R} to a
# program's sources and call version_use(target).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VERSION_R ${CMAKE_CURRENT_BINARY_DIR}/version.r)
execute_process(
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/../tools/make-vers.py
            ${VERSION_R} ${CMAKE_CURRENT_BINARY_DIR}/version.h
    OUTPUT_VARIABLE ENFORCER_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE version_result)
if(NOT version_result EQUAL 0)
    message(FATAL_ERROR "tools/make-vers.py failed")
endif()
message(STATUS "Enforcer version ${ENFORCER_VERSION}")

function(version_use target)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endfunction()
