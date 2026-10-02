# The shared test harness. It is a separate library so that every suite, and any
# child process a suite spawns, links exactly the same runner and reporting.
add_library(dce_test_support STATIC harness.cpp)

target_include_directories(dce_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(dce_test_support PUBLIC dce)

dce_strict(dce_test_support)
target_compile_features(dce_test_support PUBLIC cxx_std_20)
