# pvr-browser helper (proof of concept). Built separately from the player:
#   cmake -S browser -B build-browser -DFFMPEG6_LIB_DIR=<private FFmpeg 6 lib dir>
# SteamOS ships Qt WebEngine 6.7 linked against FFmpeg 6, which the system no
# longer has; the helper runs with a private FFmpeg 6 on its library path.
cmake_minimum_required(VERSION 3.24)
project(pvr_browser LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets)
add_executable(pvr-browser pvr_browser.cpp)
target_link_libraries(pvr-browser PRIVATE Qt6::Widgets Qt6::WebEngineWidgets rt)
if(FFMPEG6_LIB_DIR)
    target_link_options(pvr-browser PRIVATE "-Wl,-rpath-link,${FFMPEG6_LIB_DIR}")
endif()
