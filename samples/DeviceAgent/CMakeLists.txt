add_executable(edgepulse-agent Main.cpp)
target_link_libraries(edgepulse-agent PRIVATE AsynGyanis::Net AsynGyanis::Core AsynGyanis::Base OpenSSL::Crypto)
set_target_properties(edgepulse-agent PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/samples)
