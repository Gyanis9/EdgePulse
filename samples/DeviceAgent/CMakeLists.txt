add_executable(edgepulse-agent-probe Main.cpp)
target_link_libraries(edgepulse-agent-probe PRIVATE AsynGyanis::Net AsynGyanis::Core AsynGyanis::Base OpenSSL::Crypto)
set_target_properties(edgepulse-agent-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/samples)
