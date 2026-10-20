add_executable(edgepulse-server Main.cpp)
target_link_libraries(edgepulse-server PRIVATE EdgePulse::App)
edgepulse_enable_warnings(edgepulse-server)
set_target_properties(edgepulse-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
