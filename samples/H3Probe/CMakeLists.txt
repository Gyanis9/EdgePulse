add_executable(H3Probe Main.cpp)
target_link_libraries(H3Probe PRIVATE AsynGyanis::Net AsynGyanis::Core AsynGyanis::Base OpenSSL::Crypto)
set_target_properties(H3Probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/samples/bin)
