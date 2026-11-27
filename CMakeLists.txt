cmake_minimum_required(VERSION 3.16)
project(sintetizador C)

# Baixa as dependências automaticamente na primeira configuração:
#   raylib (janela/gráficos), miniaudio (áudio de entrada e saída),
#   PortMidi (MIDI). Os botões/knobs são desenhados em src/ui.c.
option(SYNTH_MIDI "Compilar com suporte a MIDI (PortMidi)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# o núcleo também vai dentro do plugin (biblioteca dinâmica)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(SYNTH_PLUGIN "Compilar o plugin CLAP (REAPER, Bitwig...)" ON)

# raylib sem o módulo de áudio: o áudio fica com o nosso miniaudio
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUSTOMIZE_BUILD ON CACHE BOOL "" FORCE)
set(USE_AUDIO OFF CACHE BOOL "" FORCE)
set(SUPPORT_MODULE_RAUDIO OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5 GIT_SHALLOW TRUE)
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.22 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(raylib)
FetchContent_Populate(miniaudio)  # só o cabeçalho

set(CORE_SOURCES
  src/params.c src/synth.c src/fx.c src/arp.c src/looper.c
  src/pitch.c src/guitar.c src/gtr_direct.c
    src/drift.c src/engine.c src/recorder.c)

# Núcleo de áudio (sem interface), usado pelo app e pelos testes
add_library(synthcore STATIC ${CORE_SOURCES} src/miniaudio_impl.c)
target_include_directories(synthcore PUBLIC src ${miniaudio_SOURCE_DIR})
# dentro dos plugins, só a entrada do plugin fica visível para o host
set_target_properties(synthcore PROPERTIES C_VISIBILITY_PRESET hidden)
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

# ASIO (só Windows): drivers de baixa latência das interfaces de áudio.
# Usa o PortAudio com o SDK ASIO 2.3 da Steinberg, sob a licença GPLv3
# (por isso o projeto inteiro é GPLv3). Por padrão o SDK vem do espelho
# jcelerier/asio-2; para usar o ZIP oficial da Steinberg já extraído:
#   -DASIOSDK_DIR=C:/caminho/asiosdk
if(WIN32)
  option(SYNTH_ASIO "Suporte a drivers ASIO (SDK da Steinberg, GPLv3)" ON)
  set(ASIOSDK_DIR "" CACHE PATH "Pasta do SDK ASIO (vazio = baixar)")
endif()
if(WIN32 AND SYNTH_ASIO)
  enable_language(CXX)
  if(ASIOSDK_DIR)
    set(ASIO_ROOT ${ASIOSDK_DIR})
  else()
    FetchContent_Declare(asiosdk
      GIT_REPOSITORY https://github.com/jcelerier/asio-2.git
      GIT_TAG 1bc070d1c7b8f0bf92f9d3b7c72e5880a7dcffdb)
    FetchContent_Populate(asiosdk)
    set(ASIO_ROOT ${asiosdk_SOURCE_DIR})
  endif()
  # mesmo conserto que o PortAudio aplica (new[] liberado com delete)
  file(READ "${ASIO_ROOT}/host/pc/asiolist.cpp" _asiolist)
  string(REPLACE "delete lpdrv" "delete[] lpdrv" _asiolist "${_asiolist}")
  file(WRITE "${CMAKE_BINARY_DIR}/asio_patched/asiolist.cpp" "${_asiolist}")
  add_library(ASIO::host INTERFACE IMPORTED)
  target_sources(ASIO::host INTERFACE
    "${ASIO_ROOT}/common/asio.cpp"
    "${ASIO_ROOT}/host/asiodrivers.cpp"
    "${CMAKE_BINARY_DIR}/asio_patched/asiolist.cpp")
  target_include_directories(ASIO::host INTERFACE
    "${ASIO_ROOT}/common" "${ASIO_ROOT}/host" "${ASIO_ROOT}/host/pc")
  target_link_libraries(ASIO::host INTERFACE ole32 uuid)

  # PortAudio só com o ASIO: o resto do áudio continua no miniaudio
  set(PA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(PA_USE_ASIO ON CACHE BOOL "" FORCE)
  foreach(api DS WMME WASAPI WDMKS WDMKS_DEVICE_INFO)
    set(PA_USE_${api} OFF CACHE BOOL "" FORCE)
  endforeach()
  FetchContent_Declare(portaudio
    GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
    GIT_TAG e8138474e338f545361065776a30878022c26106)
  FetchContent_MakeAvailable(portaudio)
  target_sources(synthcore PRIVATE src/audio_asio.c)
  target_compile_definitions(synthcore PRIVATE HAVE_ASIO)
  target_link_libraries(synthcore PUBLIC portaudio)
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

# Plugin CLAP: o mesmo motor, dentro da DAW (VirtualSynth.clap)
if(SYNTH_PLUGIN)
  FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.2 GIT_SHALLOW TRUE)
  FetchContent_Populate(clap)  # só os cabeçalhos
  add_library(vsynth_clap MODULE src/plugin_clap.c)
  target_include_directories(vsynth_clap PRIVATE ${clap_SOURCE_DIR}/include)
  target_link_libraries(vsynth_clap PRIVATE synthcore)
  set_target_properties(vsynth_clap PROPERTIES OUTPUT_NAME "VirtualSynth" C_VISIBILITY_PRESET hidden)
  if(APPLE)
    # no macOS o .clap é um bundle: VirtualSynth.clap/Contents/MacOS/VirtualSynth
    set_target_properties(vsynth_clap PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION clap PREFIX ""
      MACOSX_BUNDLE_GUI_IDENTIFIER com.vdonasan.virtualsynth MACOSX_BUNDLE_BUNDLE_NAME VirtualSynth
      MACOSX_BUNDLE_BUNDLE_VERSION 1.0.7 MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0.7)
  else()
    set_target_properties(vsynth_clap PROPERTIES PREFIX "" SUFFIX ".clap")
  endif()
  if(MINGW)
    target_link_options(vsynth_clap PRIVATE -static -static-libgcc)
  elseif(UNIX AND NOT APPLE)
    target_link_options(vsynth_clap PRIVATE -Wl,--no-undefined)
  endif()
  if(NOT MSVC)
    target_compile_options(vsynth_clap PRIVATE -Wall -Wextra)
  endif()
  # carrega o plugin como uma DAW e confere o básico
  add_executable(test_clap_host tests/test_clap_host.c)
  target_include_directories(test_clap_host PRIVATE ${clap_SOURCE_DIR}/include)
  if(UNIX)
    target_link_libraries(test_clap_host PRIVATE m ${CMAKE_DL_LIBS})
  endif()
  if(MINGW)
    target_link_options(test_clap_host PRIVATE -static)
  endif()
  add_dependencies(test_clap_host vsynth_clap)
  add_test(NAME clap_host COMMAND test_clap_host $<TARGET_FILE:vsynth_clap>)

  # VST3: o mesmo plugin embrulhado pelo clap-wrapper (free-audio), que
  # baixa o SDK VST3 da Steinberg (MIT). VirtualSynth.vst3
  option(SYNTH_VST3 "Compilar também o plugin VST3 (precisa de C++)" ON)
  if(SYNTH_VST3)
    enable_language(CXX)
    if(APPLE)
      enable_language(OBJC OBJCXX) # o clap-wrapper tem partes em Objective-C++ no Mac
    endif()
    set(CMAKE_CXX_STANDARD 17)
    set(CLAP_SDK_ROOT ${clap_SOURCE_DIR})
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)
    set(CLAP_WRAPPER_DONT_ADD_TARGETS TRUE CACHE BOOL "" FORCE)
    FetchContent_Declare(clapwrapper
      GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
      GIT_TAG v0.12.1 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(clapwrapper)
    add_library(vsynth_vst3 MODULE src/plugin_clap.c)
    target_include_directories(vsynth_vst3 PRIVATE ${clap_SOURCE_DIR}/include)
    target_link_libraries(vsynth_vst3 PRIVATE synthcore)
    set_target_properties(vsynth_vst3 PROPERTIES C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden PREFIX "")
    target_add_vst3_wrapper(TARGET vsynth_vst3 OUTPUT_NAME "VirtualSynth"
      SUPPORTS_ALL_NOTE_EXPRESSIONS FALSE
      BUNDLE_IDENTIFIER com.vdonasan.virtualsynth.vst3 BUNDLE_VERSION 1.0.7)
    if(MINGW)
      target_link_options(vsynth_vst3 PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
  endif()
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
