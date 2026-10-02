cmake_minimum_required(VERSION 3.20)

project(DownstreamConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Nothing but the installed package. The package config is responsible for every
# transitive dependency of its own imported target, including Threads, which the
# static library names in its interface: a consumer that had to call
# find_package(Threads) itself would be working around a broken export.
find_package(DataCenterEvolution CONFIG REQUIRED)

if(NOT DataCenterEvolution_VERSION)
  message(FATAL_ERROR "the installed package did not report its own version")
endif()

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE DataCenterEvolution::dce)

# Strict warnings for the consumer's own translation unit.
if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# The version the package reported is the version the program prints on its
# first line, so that line proves the config file and the binary agree.
target_compile_definitions(downstream_consumer PRIVATE
  DCE_CONSUMER_EXPECTED_VERSION="${DataCenterEvolution_VERSION}")

enable_testing()
add_test(NAME downstream_consumer COMMAND downstream_consumer)
