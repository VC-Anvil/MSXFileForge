cmake_minimum_required(VERSION 3.16)
project(MSXFileForge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- core library (no GUI deps) ---
add_library(msxcore STATIC
  core/disk_image.cpp core/fat12.cpp core/msxfile.cpp core/rom.cpp core/cas.cpp)
target_include_directories(msxcore PUBLIC core)

# --- CLI ---
add_executable(msxforge cli/main.cpp)
target_link_libraries(msxforge PRIVATE msxcore)

# --- dsk2dmk test tool ---
add_executable(dsk2dmk tools/dsk2dmk.cpp)

# --- GUI (Dear ImGui + SDL2 + OpenGL3) ---
set(IMGUI libs/imgui)
add_executable(msxforge-gui
  gui/main.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp)
target_include_directories(msxforge-gui PRIVATE ${IMGUI} libs/SDL2/include)
target_compile_definitions(msxforge-gui PRIVATE SDL_MAIN_HANDLED)

find_package(OpenGL REQUIRED)
# Link SDL2 directly against the bundled import DLL (MinGW can consume it).
target_link_libraries(msxforge-gui PRIVATE
  msxcore
  ${CMAKE_SOURCE_DIR}/libs/SDL2/lib/SDL2.dll
  OpenGL::GL
  comdlg32)

add_custom_command(TARGET msxforge-gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/libs/SDL2/lib/SDL2.dll $<TARGET_FILE_DIR:msxforge-gui>)
