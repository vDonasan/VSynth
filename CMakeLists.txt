cmake_minimum_required(VERSION 3.16)
project(sintetizador C)

# Baixa as dependências automaticamente na primeira configuração:
#   raylib (janela/gráficos), raygui (botões/sliders),
#   miniaudio (áudio de entrada e saída), PortMidi (MIDI).
option(SYNTH_MIDI "Compilar com suporte a MIDI (PortMidi)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# raylib sem o módulo de áudio: o áudio fica com o nosso miniaudio
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUSTOMIZE_BUILD ON CACHE BOOL "" FORCE)
set(USE_AUDIO OFF CACHE BOOL "" FORCE)
set(SUPPORT_MODULE_RAUDIO OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5 GIT_SHALLOW TRUE)
FetchContent_Declare(raygui
  GIT_REPOSITORY https://github.com/raysan5/raygui.git
  GIT_TAG 4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.22 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(raylib)
FetchContent_Populate(raygui)     # só o cabeçalho
FetchContent_Populate(miniaudio)  # só o cabeçalho

set(CORE_SOURCES
  src/params.c src/synth.c src/fx.c src/arp.c src/looper.c
  src/pitch.c src/guitar.c src/engine.c src/recorder.c)

# Núcleo de áudio (sem interface), usado pelo app e pelos testes
add_library(synthcore STATIC ${CORE_SOURCES} src/miniaudio_impl.c)
target_include_directories(synthcore PUBLIC src ${miniaudio_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(synthcore PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(synthcore PUBLIC m ${CMAKE_DL_LIBS})
endif()

# Fontes Inter embutidas no executável (licença em assets/fonts/OFL-Inter.txt)
set(FONT_SOURCES)
foreach(f Regular SemiBold Bold)
  string(TOLOWER ${f} lower)
  set(out ${CMAKE_BINARY_DIR}/font_inter_${lower}.c)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/assets/fonts/Inter-${f}.ttf
            -DOUT=${out} -DNAME=font_inter_${lower} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/fonts/Inter-${f}.ttf ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embutindo a fonte Inter ${f}")
  list(APPEND FONT_SOURCES ${out})
endforeach()

add_executable(synth src/main.c src/gui.c src/ui.c src/midi.c ${FONT_SOURCES})
target_include_directories(synth PRIVATE ${raygui_SOURCE_DIR}/src ${raygui_SOURCE_DIR}/styles)
# raygui é só cabeçalho e gera avisos inofensivos
set_source_files_properties(src/gui.c PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wno-unused-parameter -Wno-unused-result>")
target_link_libraries(synth PRIVATE synthcore raylib)

if(SYNTH_MIDI)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_PORTMIDI_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_PMDEFAULTS OFF CACHE BOOL "" FORCE)
  set(BUILD_JAVA_NATIVE_INTERFACE OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(portmidi
    GIT_REPOSITORY https://github.com/PortMidi/portmidi.git
    GIT_TAG v2.0.4 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(portmidi)
  target_compile_definitions(synth PRIVATE HAVE_PORTMIDI)
  target_link_libraries(synth PRIVATE portmidi)
endif()

if(MSVC)
  target_compile_options(synthcore PRIVATE /W3)
else()
  target_compile_options(synthcore PRIVATE -Wall -Wextra)
  target_compile_options(synth PRIVATE -Wall -Wextra)
endif()

# No Windows (MinGW), embute as bibliotecas do compilador no .exe
if(MINGW)
  target_link_options(synth PRIVATE -static -mwindows)
endif()

# Testes (não precisam de placa de som nem de janela)
enable_testing()
add_executable(test_dsp tests/test_dsp.c)
target_link_libraries(test_dsp PRIVATE synthcore)
add_test(NAME dsp COMMAND test_dsp)
if(MINGW)
  target_link_options(test_dsp PRIVATE -static)
endif()

# Saída MIDI com um PortMidi falso (testa os bytes enviados sem hardware)
add_executable(test_midi_out tests/test_midi_out.c src/midi.c)
target_include_directories(test_midi_out PRIVATE tests/fake_portmidi)
target_compile_definitions(test_midi_out PRIVATE HAVE_PORTMIDI)
target_link_libraries(test_midi_out PRIVATE synthcore)
add_test(NAME midi_out COMMAND test_midi_out)
if(MINGW)
  target_link_options(test_midi_out PRIVATE -static)
endif()

# Copia os presets de fábrica para junto do executável
add_custom_command(TARGET synth POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:synth>/presets)
