# Headless domain-core tests. Each target links Qt6::Test and cutpilot-core only,
# so the tested logic stays free of any GUI dependency. Sibling executables are
# added a line at a time as new core surfaces land.

qt_add_executable(tst_nodegraph tst_nodegraph.cpp)
target_link_libraries(tst_nodegraph PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.nodegraph COMMAND tst_nodegraph)

qt_add_executable(tst_spatialindex tst_spatialindex.cpp)
target_link_libraries(tst_spatialindex PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.spatialindex COMMAND tst_spatialindex)

qt_add_executable(tst_commandstack tst_commandstack.cpp)
target_link_libraries(tst_commandstack PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.commandstack COMMAND tst_commandstack)

qt_add_executable(tst_alignmentguides tst_alignmentguides.cpp)
target_link_libraries(tst_alignmentguides PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.alignmentguides COMMAND tst_alignmentguides)

qt_add_executable(tst_connections tst_connections.cpp)
target_link_libraries(tst_connections PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.connections COMMAND tst_connections)

qt_add_executable(tst_connectorpath tst_connectorpath.cpp)
target_link_libraries(tst_connectorpath PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.connectorpath COMMAND tst_connectorpath)
