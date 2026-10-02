# Worked examples of the evolution boundary, built as part of the main tree.
#
# Both executables use nothing but the public API in include/dce plus the
# in-tree target, so building them is also a check that the public headers are
# self-sufficient and warning-clean under dce_strict().
add_executable(evolve_fleet evolve_fleet.cpp)
target_link_libraries(evolve_fleet PRIVATE dce)
dce_strict(evolve_fleet)

add_executable(inspect_plan inspect_plan.cpp)
target_link_libraries(inspect_plan PRIVATE dce)
dce_strict(inspect_plan)

# examples/downstream-consumer is deliberately NOT part of this build. It is a
# standalone project that must be configured against an *installed*
# DataCenterEvolution package with find_package(DataCenterEvolution CONFIG),
# so adding it here would let a missing or broken package export go unnoticed.
