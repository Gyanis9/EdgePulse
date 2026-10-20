add_library(edgepulse-app STATIC
        Application.cpp
        ApplicationConfiguration.cpp
        ListenerAssembly.cpp)
add_library(EdgePulse::App ALIAS edgepulse-app)

target_include_directories(edgepulse-app PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-app
        PUBLIC EdgePulse::Common
               EdgePulse::Web
               AsynGyanis::Net
               AsynGyanis::Base
               AsynGyanis::Core)
edgepulse_enable_warnings(edgepulse-app)

add_executable(edgepulse-server Main.cpp)
target_link_libraries(edgepulse-server PRIVATE EdgePulse::App)
edgepulse_enable_warnings(edgepulse-server)
set_target_properties(edgepulse-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
