add_library(edgepulse-plugins STATIC
        AuthPlugin/AuthController.cpp
        DevicePlugin/AgentController.cpp
        DevicePlugin/DeviceController.cpp
        DevicePlugin/NodeJson.cpp)
add_library(EdgePulse::Plugins ALIAS edgepulse-plugins)

target_include_directories(edgepulse-plugins PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-plugins
        PUBLIC EdgePulse::Core
               AsynGyanis::Net
               AsynGyanis::Core
               AsynGyanis::Base)
edgepulse_enable_warnings(edgepulse-plugins)
