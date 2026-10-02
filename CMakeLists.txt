cmake_minimum_required(VERSION 3.20)

project(DataCenterEvolution
  VERSION 1.0.1
  DESCRIPTION "DCCP boundary governing live evolution of a multi-site data-center control plane"
  HOMEPAGE_URL "https://github.com/summonlabs/Data-Center-Evolution"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(DCE_BUILD_TESTS "Build the Data Center Evolution test suite" ON)
option(DCE_BUILD_TOOLS "Build the command line tools and reference processes" ON)
option(DCE_BUILD_EXAMPLES "Build the examples" ON)
option(DCE_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(DCE_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
# AddressSanitizer is available where the toolchain ships the matching runtime.
# On a Visual Studio installation without the x64 AddressSanitizer component
# this option cannot link; Debug then provides /RTC1 runtime checks instead.
# See docs/VALIDATION.md for the exact proof boundary on each platform.
option(DCE_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(DCE_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer where the toolchain supports it" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning configuration. First-party code builds with zero warnings.
# ---------------------------------------------------------------------------
function(dce_strict target)
  # Portable C stdio is used deliberately for the durability primitives, so the
  # MSVC "unsafe" deprecation shim for fopen and friends is not applicable.
  target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)
    if(DCE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-qual -Wunused
      -Woverloaded-virtual -Wold-style-cast -Wdouble-promotion -Wformat=2
      -Wconversion -Wsign-conversion)
    if(DCE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(DCE_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
  if(DCE_ENABLE_UBSAN AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=undefined)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library: the evolution boundary
# ---------------------------------------------------------------------------
add_library(dce STATIC
  src/status.cpp
  src/text.cpp
  src/digest.cpp
  src/version.cpp
  src/canonical.cpp
  src/codec.cpp
  src/capability.cpp
  src/evidence.cpp
  src/compatibility.cpp
  src/lifecycle.cpp
  src/random.cpp
  src/plan.cpp
  src/rollout.cpp
  src/authority.cpp
  src/reconcile.cpp
  src/validate.cpp
  src/synthetic.cpp
  src/store.cpp
  src/platform.cpp
  src/protocol.cpp
  src/coordinator.cpp
  src/site_node.cpp
)
add_library(DataCenterEvolution::dce ALIAS dce)

target_include_directories(dce
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

dce_strict(dce)
target_compile_features(dce PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(dce PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(dce PUBLIC ws2_32)
endif()

set_target_properties(dce PROPERTIES
  OUTPUT_NAME dce
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS dce
  EXPORT DataCenterEvolutionTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT DataCenterEvolutionTargets
  FILE DataCenterEvolutionTargets.cmake
  NAMESPACE DataCenterEvolution::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DataCenterEvolution)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DataCenterEvolutionConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/DataCenterEvolutionConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DataCenterEvolution)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/DataCenterEvolutionConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/DataCenterEvolutionConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/DataCenterEvolutionConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DataCenterEvolution)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/DataCenterEvolution)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(DCE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(DCE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# The multi-process harness is shared by the end-to-end tests and the benchmarks.
if(DCE_BUILD_TESTS OR DCE_BUILD_BENCHMARKS)
  add_subdirectory(tests/support)
endif()
if(DCE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(DCE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
