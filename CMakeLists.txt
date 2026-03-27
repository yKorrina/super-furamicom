cmake_minimum_required(VERSION 3.10)
project(SuperFuramicom)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

# Find SDL2
find_package(SDL2 REQUIRED)

# Source files in src/ directory
file(GLOB SOURCES "src/*.cpp")
add_executable(snes_emu ${SOURCES})

target_include_directories(snes_emu PRIVATE src ${SDL2_INCLUDE_DIRS})
target_link_libraries(snes_emu ${SDL2_LIBRARIES})

if(TARGET SDL2::SDL2)
    target_link_libraries(snes_emu SDL2::SDL2)
endif()
