# hooks_embed(target): assemble the write hooks (common/h68k.s and
# common/hnat.s) and make their bytes available to the target as
# h68k_code.h (array h68k_code) and hnat_code.h (array hnat_code). Several
# targets in one directory share the generated headers.
set(HOOKS_COMMON ${CMAKE_CURRENT_LIST_DIR})

function(hooks_embed target)
    if(NOT TARGET hooks_code)
        hooks_generate()
    endif()
    add_dependencies(${target} hooks_code)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${HOOKS_COMMON})
    # CurrentExecutionLevel, for the engine's fault handler.
    target_link_libraries(${target} DriverServicesLib)
endfunction()

function(hooks_generate)
    get_filename_component(toolbin ${CMAKE_C_COMPILER} DIRECTORY)
    set(headers)
    foreach(name h68k hnat)
        set(obj ${CMAKE_CURRENT_BINARY_DIR}/${name}.o)
        set(bin ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin)
        set(hdr ${CMAKE_CURRENT_BINARY_DIR}/${name}_code.h)
        add_custom_command(
            OUTPUT ${hdr}
            COMMAND ${toolbin}/powerpc-apple-macos-as -mppc ${HOOKS_COMMON}/${name}.s -o ${obj}
            COMMAND ${toolbin}/powerpc-apple-macos-objcopy -O binary -j .text ${obj} ${bin}
            COMMAND ${CMAKE_COMMAND} -DIN=${bin} -DOUT=${hdr} -DNAME=${name}_code
                    -P ${HOOKS_COMMON}/embed.cmake
            DEPENDS ${HOOKS_COMMON}/${name}.s ${HOOKS_COMMON}/embed.cmake
            COMMENT "Assembling ${name}.s")
        list(APPEND headers ${hdr})
    endforeach()
    add_custom_target(hooks_code DEPENDS ${headers})
endfunction()
