# Benchmarks for the DCE evolution boundary.
#
# This target is a measurement tool, not a test. It prints timing figures taken
# on the machine that runs it, so it is deliberately NOT registered with CTest:
# a timing number must never be able to fail a correctness build.
#
# It links the in-tree library target `dce` and builds under exactly the same
# strict warning configuration as the rest of the project.

add_executable(dce_bench bench_main.cpp)

target_link_libraries(dce_bench PRIVATE dce)

dce_strict(dce_bench)
