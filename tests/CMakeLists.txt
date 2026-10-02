# One executable per suite. A suite is a single translation unit whose cases
# register themselves before main runs, so registration order is deterministic
# and no test needs a discovery step.
function(dce_add_test target_name source_file)
  add_executable(${target_name} ${source_file})
  target_link_libraries(${target_name} PRIVATE dce_test_support dce)
  dce_strict(${target_name})
  add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

# Suites that drive the shipped executable need to know where the build put it.
# The build directory is passed as a forward-slash CMake path, which is safe in
# a compile definition on every platform; the suites also fall back to looking
# next to themselves.
function(dce_add_cli_test target_name source_file)
  dce_add_test(${target_name} ${source_file})
  target_compile_definitions(${target_name} PRIVATE "DCE_CLI_BUILD_DIR=\"${CMAKE_BINARY_DIR}\"")
endfunction()

# Primitives
dce_add_test(dce_test_primitives test_primitives.cpp)
dce_add_test(dce_test_primitives_adversarial test_primitives_adversarial.cpp)

# Model
dce_add_test(dce_test_capability test_capability.cpp)
dce_add_test(dce_test_compatibility test_compatibility.cpp)
dce_add_test(dce_test_plan test_plan.cpp)
dce_add_test(dce_test_lifecycle test_lifecycle.cpp)
dce_add_test(dce_test_rollout test_rollout.cpp)
dce_add_test(dce_test_authority test_authority.cpp)
dce_add_test(dce_test_reconcile test_reconcile.cpp)
dce_add_test(dce_test_validate test_validate.cpp)
dce_add_test(dce_test_property test_property.cpp)
dce_add_test(dce_test_adversarial test_adversarial.cpp)

# Durability
dce_add_test(dce_test_persistence test_persistence.cpp)

# The runtime, the shipped executable and real independent processes
dce_add_cli_test(dce_test_cli test_cli.cpp)
dce_add_cli_test(dce_test_multiprocess test_multiprocess.cpp)
