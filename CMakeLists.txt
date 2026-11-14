cmake_minimum_required(VERSION 3.16)
project(CorsairCommanderCommander VERSION 1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT WIN32)
  message(FATAL_ERROR "Windows only")
endif()

set(APP_ICON ${CMAKE_SOURCE_DIR}/packaging/app.ico)
set(APP_MANIFEST ${CMAKE_SOURCE_DIR}/packaging/app.manifest)
set(PAWNIO_MODULE ${CMAKE_SOURCE_DIR}/third_party/pawnio/AMDFamily17.bin)  # signed, LGPL-2.1 (see COPYING)
configure_file(packaging/app.rc.in ${CMAKE_BINARY_DIR}/app.rc @ONLY)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # single exe, no redistributables

add_executable(CorsairCommanderCommander WIN32
  src/app.cpp
  src/core/icue_import.cpp
  src/core/model.cpp
  src/ui/canvas.cpp
  src/ui/widgets.cpp
  src/win32/commander_pro.cpp
  src/win32/corsair_psu.cpp
  src/win32/debug_tools.cpp
  src/win32/engine.cpp
  src/win32/font_win32.cpp
  src/win32/hid.cpp
  src/win32/host_sensors.cpp
  src/win32/main_win32.cpp
  ${CMAKE_BINARY_DIR}/app.rc)
target_include_directories(CorsairCommanderCommander PRIVATE src)
target_link_libraries(CorsairCommanderCommander PRIVATE user32 gdi32 shell32 advapi32 hid setupapi)
target_compile_definitions(CorsairCommanderCommander PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _CRT_SECURE_NO_WARNINGS APP_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(CorsairCommanderCommander PRIVATE /W3 /O2 /GR-)
  target_link_options(CorsairCommanderCommander PRIVATE /OPT:REF /OPT:ICF /MANIFEST:NO)  # manifest comes from app.rc
else()
  target_compile_options(CorsairCommanderCommander PRIVATE -O2 -Wall -Wno-unused-parameter)
  target_link_options(CorsairCommanderCommander PRIVATE -static -s -municode)
endif()
