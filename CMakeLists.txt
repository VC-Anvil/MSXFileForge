# MSXFileForge - cross-platform build (Windows / Linux / macOS).
cmake_minimum_required(VERSION 3.16)
project(MSXFileForge CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- core library (portable, no GUI deps) ---
add_library(msxcore STATIC
  core/disk_image.cpp core/fat12.cpp core/msxfile.cpp core/rom.cpp core/cas.cpp core/util.cpp core/tape.cpp
  core/partition.cpp)
target_include_directories(msxcore PUBLIC core)

# --- CLI ---
add_executable(msxforge cli/main.cpp)
target_link_libraries(msxforge PRIVATE msxcore)

# --- dsk2dmk test tool ---
add_executable(dsk2dmk tools/dsk2dmk.cpp)
target_link_libraries(dsk2dmk PRIVATE msxcore)

# --- tests (core library, no GUI deps): ctest --test-dir build ---
option(MSXFF_BUILD_TESTS "Build the core test suite" ON)
if(MSXFF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# --- GUI (Dear ImGui + SDL2 + OpenGL3 + tinyfiledialogs) ---
# Turn off with -DMSXFF_BUILD_GUI=OFF to build only the core, CLI and tests
# (no SDL2 / OpenGL needed: handy for headless CI and servers).
option(MSXFF_BUILD_GUI "Build the Dear ImGui desktop app" ON)
if(MSXFF_BUILD_GUI)
set(IMGUI ${CMAKE_SOURCE_DIR}/libs/imgui)
add_executable(msxforge-gui
  gui/main.cpp
  libs/tinyfiledialogs.c
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp)
target_include_directories(msxforge-gui PRIVATE ${IMGUI} ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(msxforge-gui PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(msxforge-gui PRIVATE msxcore)

find_package(OpenGL REQUIRED)
target_link_libraries(msxforge-gui PRIVATE OpenGL::GL)

if(WIN32)
  # Bundled SDL2 (dev headers + import DLL) so Windows devs need nothing installed.
  target_include_directories(msxforge-gui PRIVATE ${CMAKE_SOURCE_DIR}/libs/SDL2/include)
  # MSVC links the import lib; comdlg32/ole32/shell32/user32 are for tinyfiledialogs.
  target_link_libraries(msxforge-gui PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/SDL2/lib/SDL2.lib
    comdlg32 ole32 shell32 user32)
  add_custom_command(TARGET msxforge-gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/libs/SDL2/lib/SDL2.dll $<TARGET_FILE_DIR:msxforge-gui>)
else()
  # Linux / macOS: use the system SDL2 (apt install libsdl2-dev / brew install sdl2).
  find_package(SDL2 REQUIRED)
  if(TARGET SDL2::SDL2)
    target_link_libraries(msxforge-gui PRIVATE SDL2::SDL2)         # shared (brew/apt)
  elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(msxforge-gui PRIVATE SDL2::SDL2-static)  # static (cross build)
  else()
    target_include_directories(msxforge-gui PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(msxforge-gui PRIVATE ${SDL2_LIBRARIES})
  endif()
endif()
endif() # MSXFF_BUILD_GUI
