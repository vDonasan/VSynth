# Converte um arquivo binário (ex.: uma fonte .ttf) num array C, para o
# executável não depender de arquivos ao lado dele.
#   cmake -DIN=arquivo.ttf -DOUT=arquivo.c -DNAME=nome -P embed.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR size "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}" "/* Gerado a partir de ${IN} — não edite. */\nconst unsigned char ${NAME}[] = {\n${bytes}\n};\nconst unsigned int ${NAME}_size = ${size};\n")
