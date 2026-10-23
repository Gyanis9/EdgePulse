add_executable(FirmwareVerifier Main.cpp)
target_link_libraries(FirmwareVerifier PRIVATE EdgePulse::Core OpenSSL::Crypto)
set_target_properties(FirmwareVerifier PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/samples)
