# Physical Location Registry - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the examples, the benchmarks and the tests each carry
# exactly the options they need, and nothing this project uses for itself can
# leak into the installed package.

cmake_minimum_required(VERSION 3.20)

project(PhysicalLocationRegistry
        VERSION 1.0.0
        DESCRIPTION "Stable physical addressing for DCCP facility locations"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build physical_location_registry as a shared library instead of a static one" OFF)

option(PLR_BUILD_TESTS "Build the Physical Location Registry test suite" ON)
option(PLR_BUILD_TOOLS "Build the Physical Location Registry command line tools" ON)
option(PLR_BUILD_EXAMPLES "Build the Physical Location Registry example programs" ON)
option(PLR_BUILD_BENCHMARKS "Build the Physical Location Registry benchmarks" ON)
option(PLR_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(PLR_SANITIZERS "Build with AddressSanitizer, and UndefinedBehaviorSanitizer on GCC and Clang" OFF)
option(PLR_ANALYZE "Build with the MSVC static analyzer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCXXCompilerFlag)
include(CTest)

set(PLR_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/PhysicalLocationRegistry"
    CACHE STRING "Install destination of the Physical Location Registry CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it is part of the installed package.
function(plr_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(PLR_WARNINGS_AS_ERRORS AND NOT PLR_ANALYZE)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/RTC1>)
    target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=2>)
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Release>:/O2>
      $<$<CONFIG:Release>:/Gy>
      $<$<CONFIG:Release>:/Gw>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(PLR_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(MSVC AND PLR_ANALYZE)
    target_compile_options(${target} PRIVATE /analyze /analyze:WX-)
  endif()
  if(PLR_SANITIZERS)
    target_compile_options(${target} PRIVATE ${PLR_SANITIZER_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${PLR_SANITIZER_LINK_OPTIONS})
  endif()
endfunction()

function(plr_add_executable target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE physical_location_registry)
  plr_configure_target(${target})
endfunction()

# ---------------------------------------------------------------------------
# Sanitizers and static analysis
# ---------------------------------------------------------------------------

if(PLR_SANITIZERS)
  if(MSVC)
    check_cxx_compiler_flag("/fsanitize=address" PLR_COMPILER_HAS_ASAN)
    if(NOT PLR_COMPILER_HAS_ASAN)
      message(FATAL_ERROR
        "PLR_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: a program built with /fsanitize=address does not "
        "compile and link here. Either the toolset predates AddressSanitizer support "
        "(Visual Studio 2019 16.9) or the optional C++ AddressSanitizer component is not "
        "installed. Reconfigure with -DPLR_SANITIZERS=OFF or install that component.")
    endif()
    set(PLR_SANITIZER_COMPILE_OPTIONS /fsanitize=address /Zi)
    set(PLR_SANITIZER_LINK_OPTIONS /fsanitize=address)
    message(STATUS "PhysicalLocationRegistry: AddressSanitizer enabled (MSVC)")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    check_cxx_compiler_flag("-fsanitize=address,undefined" PLR_COMPILER_HAS_ASAN_UBSAN)
    if(NOT PLR_COMPILER_HAS_ASAN_UBSAN)
      message(FATAL_ERROR
        "PLR_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: the sanitizer runtime is unavailable. "
        "Reconfigure with -DPLR_SANITIZERS=OFF.")
    endif()
    set(PLR_SANITIZER_COMPILE_OPTIONS -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(PLR_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
    message(STATUS "PhysicalLocationRegistry: AddressSanitizer and UndefinedBehaviorSanitizer enabled")
  else()
    message(FATAL_ERROR
      "PLR_SANITIZERS=ON is supported with MSVC (/fsanitize=address) and with GCC or Clang "
      "(-fsanitize=address,undefined); the configured compiler is "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. "
      "Reconfigure with -DPLR_SANITIZERS=OFF.")
  endif()
endif()

# ---------------------------------------------------------------------------
# physical_location_registry
# ---------------------------------------------------------------------------

set(PLR_SOURCES
  src/version.cpp
  src/result.cpp
  src/text.cpp
  src/kind.cpp
  src/address.cpp
  src/rack_unit.cpp
  src/lifecycle.cpp
  src/provenance.cpp
  src/limits.cpp
  src/location.cpp
  src/digest.cpp
  src/snapshot_codec.cpp
  src/snapshot.cpp
  src/registry_diff.cpp
  src/explanation.cpp
  src/registry.cpp
  src/registry_mutations.cpp
  src/platform.cpp
  src/store.cpp)

add_library(physical_location_registry ${PLR_SOURCES})
add_library(SummonSoftwareLabs::PhysicalLocationRegistry ALIAS physical_location_registry)

set_target_properties(physical_location_registry PROPERTIES
  # Consumers link SummonSoftwareLabs::PhysicalLocationRegistry: the exported
  # name is the project name, the artifact keeps the library name.
  EXPORT_NAME PhysicalLocationRegistry
  OUTPUT_NAME physical_location_registry
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

if(BUILD_SHARED_LIBS)
  set_target_properties(physical_location_registry PROPERTIES CXX_VISIBILITY_PRESET hidden)
  target_compile_definitions(physical_location_registry PRIVATE PHYSICAL_LOCATION_REGISTRY_BUILD_SHARED)
  target_compile_definitions(physical_location_registry INTERFACE PHYSICAL_LOCATION_REGISTRY_USE_SHARED)
endif()

target_include_directories(physical_location_registry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(physical_location_registry PUBLIC cxx_std_20)

plr_configure_target(physical_location_registry)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

if(PLR_BUILD_TOOLS)
  plr_add_executable(plr-cli tools/plr_cli.cpp tools/tool_support.cpp)
  plr_add_executable(plr-store-agent tools/store_agent.cpp tools/tool_support.cpp)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(PLR_BUILD_EXAMPLES)
  file(GLOB PLR_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
  foreach(example_source IN LISTS PLR_EXAMPLE_SOURCES)
    get_filename_component(example_stem "${example_source}" NAME_WE)
    plr_add_executable(plr-example-${example_stem} "${example_source}")
  endforeach()
  if(NOT PLR_EXAMPLE_SOURCES)
    message(STATUS "PhysicalLocationRegistry: PLR_BUILD_EXAMPLES is ON but examples/ has no program yet")
  endif()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(PLR_BUILD_BENCHMARKS)
  plr_add_executable(plr-benchmarks benchmarks/benchmark_locations.cpp)
  target_link_libraries(plr-benchmarks PRIVATE physical_location_registry_test_support)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(BUILD_TESTING AND PLR_BUILD_TESTS)
  # One executable per translation unit in tests/. tests/support is the shared
  # harness; tests/package_consumer is a standalone downstream project that is
  # configured separately against the installed package.
  file(GLOB PLR_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  file(GLOB PLR_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp")

  if(PLR_TEST_SUPPORT_SOURCES)
    add_library(physical_location_registry_test_support STATIC ${PLR_TEST_SUPPORT_SOURCES})
    set_target_properties(physical_location_registry_test_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(physical_location_registry_test_support PUBLIC physical_location_registry)
    target_include_directories(physical_location_registry_test_support PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/tests"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
    plr_configure_target(physical_location_registry_test_support)
  endif()

  foreach(test_source IN LISTS PLR_TEST_SOURCES)
    # tests/test_ids.cpp -> plr-test-ids
    get_filename_component(test_stem "${test_source}" NAME_WE)
    string(REGEX REPLACE "^test_" "" test_suffix "${test_stem}")
    set(test_target "plr-test-${test_suffix}")

    plr_add_executable(${test_target} "${test_source}")
    if(TARGET physical_location_registry_test_support)
      target_link_libraries(${test_target} PRIVATE physical_location_registry_test_support)
    endif()

    # The multi-process suite drives the real tools, so they must exist first.
    if(TARGET plr-store-agent AND TARGET plr-cli)
      add_dependencies(${test_target} plr-store-agent plr-cli)
    endif()

    if(test_stem STREQUAL "test_multiprocess" AND TARGET plr-store-agent AND TARGET plr-cli)
      add_test(NAME ${test_target}
               COMMAND ${test_target} multiprocess
                       --agent $<TARGET_FILE:plr-store-agent>
                       --cli $<TARGET_FILE:plr-cli>)
    else()
      add_test(NAME ${test_target} COMMAND ${test_target})
    endif()
  endforeach()
endif()

# The benchmark target links the test support library, which is only defined
# when the tests are enabled; keep the two switches consistent.
if(PLR_BUILD_BENCHMARKS AND NOT TARGET physical_location_registry_test_support)
  message(FATAL_ERROR
    "PLR_BUILD_BENCHMARKS=ON requires the test support library, which is built when "
    "BUILD_TESTING and PLR_BUILD_TESTS are ON. Reconfigure with -DPLR_BUILD_TESTS=ON.")
endif()

# ---------------------------------------------------------------------------
# Installation and packaging
# ---------------------------------------------------------------------------

install(TARGETS physical_location_registry
        EXPORT PhysicalLocationRegistryTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT PhysicalLocationRegistryTargets
        NAMESPACE SummonSoftwareLabs::
        DESTINATION ${PLR_INSTALL_CMAKEDIR})

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PhysicalLocationRegistryConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/PhysicalLocationRegistryConfig.cmake"
  INSTALL_DESTINATION "${PLR_INSTALL_CMAKEDIR}"
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/PhysicalLocationRegistryConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/PhysicalLocationRegistryConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/PhysicalLocationRegistryConfigVersion.cmake"
        DESTINATION ${PLR_INSTALL_CMAKEDIR})

# The build tree is usable by a sibling project without installing first.
export(EXPORT PhysicalLocationRegistryTargets
       NAMESPACE SummonSoftwareLabs::
       FILE "${CMAKE_CURRENT_BINARY_DIR}/PhysicalLocationRegistryTargets.cmake")
