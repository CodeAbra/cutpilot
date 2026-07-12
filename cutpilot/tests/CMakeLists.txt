# Headless domain-core tests. Each target links Qt6::Test and cutpilot-core only,
# so the tested logic stays free of any GUI dependency. Sibling executables are
# added a line at a time as new core surfaces land.

qt_add_executable(tst_nodegraph tst_nodegraph.cpp)
target_link_libraries(tst_nodegraph PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.nodegraph COMMAND tst_nodegraph)

qt_add_executable(tst_spatialindex tst_spatialindex.cpp)
target_link_libraries(tst_spatialindex PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.spatialindex COMMAND tst_spatialindex)
