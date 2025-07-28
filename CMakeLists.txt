cmake_minimum_required(VERSION 3.14..3.30)
project(Tori)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if (WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
endif ()

find_package(LibtorrentRasterbar CONFIG REQUIRED)

add_executable(tori main.cpp)
target_link_libraries(tori PRIVATE LibtorrentRasterbar::torrent-rasterbar)
