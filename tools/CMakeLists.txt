# The operator command line interface. The executable is named "dce" so that
# the tool, the library and the documentation all use one name.
add_executable(dce_cli dce.cpp)
set_target_properties(dce_cli PROPERTIES OUTPUT_NAME dce)
target_link_libraries(dce_cli PRIVATE dce)
dce_strict(dce_cli)
