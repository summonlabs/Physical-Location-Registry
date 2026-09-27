# Downstream consumer of the installed Physical Location Registry package.
#
# This project is deliberately outside the repository's own build: it consumes
# the installed package exactly as an unrelated DCCP repository would, using
# find_package and the exported namespace.

cmake_minimum_required(VERSION 3.20)

project(PhysicalLocationRegistryConsumer
        VERSION 1.0.0
        DESCRIPTION "Independent downstream consumer of the Physical Location Registry package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PhysicalLocationRegistry CONFIG REQUIRED)

add_executable(plr-consumer main.cpp)
target_link_libraries(plr-consumer PRIVATE SummonSoftwareLabs::PhysicalLocationRegistry)

if(MSVC)
  target_compile_options(plr-consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
else()
  target_compile_options(plr-consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

message(STATUS "PhysicalLocationRegistry version: ${PhysicalLocationRegistry_VERSION}")
message(STATUS "PhysicalLocationRegistry include dirs: ${PhysicalLocationRegistry_INCLUDE_DIRS}")
