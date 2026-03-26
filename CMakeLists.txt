cmake_minimum_required(VERSION 3.10)
project(SNES_Emulator)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Gather all source files
file(GLOB SOURCES "*.cpp")

add_executable(snes_emu ${SOURCES})