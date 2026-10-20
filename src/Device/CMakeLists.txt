add_library(edgepulse-device STATIC
        AgentController.cpp
        AgentRegistry.cpp
        DeviceController.cpp
        NodeJson.cpp
        NodeRepository.cpp
        NodeService.cpp)
add_library(EdgePulse::Device ALIAS edgepulse-device)

target_include_directories(edgepulse-device PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-device
        PUBLIC EdgePulse::Common
               EdgePulse::Web
               AsynGyanis::Net
               AsynGyanis::Core
               AsynGyanis::Base)
edgepulse_enable_warnings(edgepulse-device)
