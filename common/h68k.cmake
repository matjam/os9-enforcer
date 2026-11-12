# h68k_embed(target): assemble common/h68k.s and make its bytes available to
# the target as h68k_code.h (array h68k_code).
set(H68K_COMMON ${CMAKE_CURRENT_LIST_DIR})

function(h68k_embed target)
    get_filename_component(toolbin ${CMAKE_C_COMPILER} DIRECTORY)
    set(obj ${CMAKE_CURRENT_BINARY_DIR}/h68k.o)
    set(bin ${CMAKE_CURRENT_BINARY_DIR}/h68k.bin)
    set(hdr ${CMAKE_CURRENT_BINARY_DIR}/h68k_code.h)
    add_custom_command(
        OUTPUT ${hdr}
        COMMAND ${toolbin}/powerpc-apple-macos-as -mppc ${H68K_COMMON}/h68k.s -o ${obj}
        COMMAND ${toolbin}/powerpc-apple-macos-objcopy -O binary -j .text ${obj} ${bin}
        COMMAND ${CMAKE_COMMAND} -DIN=${bin} -DOUT=${hdr} -DNAME=h68k_code
                -P ${H68K_COMMON}/embed.cmake
        DEPENDS ${H68K_COMMON}/h68k.s ${H68K_COMMON}/embed.cmake
        COMMENT "Assembling h68k.s")
    add_custom_target(${target}_h68k DEPENDS ${hdr})
    add_dependencies(${target} ${target}_h68k)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${H68K_COMMON})
endfunction()
