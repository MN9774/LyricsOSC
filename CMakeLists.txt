cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(LyricsOSC CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(FFGL_DIR "ffgl-sdk" CACHE PATH "Carpeta del SDK FFGL de Resolume")

# Compilamos el SDK directamente junto al plugin (sin depender de sus .sln)
file(GLOB_RECURSE SDK_SRC "${FFGL_DIR}/source/lib/*.cpp")
file(GLOB_RECURSE SDK_HDR "${FFGL_DIR}/source/lib/*.h")
set(INC_DIRS "${FFGL_DIR}/source/lib")
foreach(h ${SDK_HDR})
  get_filename_component(d ${h} DIRECTORY)
  list(APPEND INC_DIRS ${d})
endforeach()
list(REMOVE_DUPLICATES INC_DIRS)

# FFGLSDK.cpp del SDK incluye otros .cpp dentro (unity build): no hay que compilarlos dos veces
file(GLOB_RECURSE UNITY_FILE "${FFGL_DIR}/source/lib/FFGLSDK.cpp")
if(UNITY_FILE)
  list(GET UNITY_FILE 0 UNITY_FILE)
  file(STRINGS "${UNITY_FILE}" UNITY_LINES REGEX "^[ \t]*#[ \t]*include")
  set(UNITY_INCLUDED "")
  foreach(line IN LISTS UNITY_LINES)
    if(line MATCHES "([A-Za-z0-9_]+\\.cpp)")
      list(APPEND UNITY_INCLUDED "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  foreach(name IN LISTS UNITY_INCLUDED)
    string(REPLACE "." "\\." esc "${name}")
    list(FILTER SDK_SRC EXCLUDE REGEX "/${esc}$")
  endforeach()
  message(STATUS "Excluidos por estar ya en FFGLSDK.cpp: ${UNITY_INCLUDED}")
endif()

if(NOT SDK_SRC)
  message(FATAL_ERROR "No se encontraron fuentes del SDK FFGL en ${FFGL_DIR}/source/lib")
endif()

find_package(GLEW REQUIRED)

add_library(LyricsOSC SHARED
  src/Lyrics.cpp
  src/LyricsCore.cpp
  src/TextRender.cpp
  ${SDK_SRC})

set_target_properties(LyricsOSC PROPERTIES PREFIX "")
target_include_directories(LyricsOSC PRIVATE ${INC_DIRS})
target_compile_definitions(LyricsOSC PRIVATE _CRT_SECURE_NO_WARNINGS GLEW_STATIC)
target_compile_options(LyricsOSC PRIVATE /utf-8 /EHsc)
target_link_options(LyricsOSC PRIVATE /EXPORT:plugMain)
target_link_libraries(LyricsOSC PRIVATE GLEW::GLEW opengl32 gdiplus ws2_32 shell32 ole32)
