# Write the bytes of IN to OUT as a C array named NAME.
#   cmake -DIN=file.bin -DOUT=file.h -DNAME=array -P embed.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR len "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}"
    "/* Generated from ${IN}; do not edit. */\n"
    "static const unsigned char ${NAME}[${len}] = {${bytes}};\n")
