add_library(edgepulse-plugins STATIC
        AlarmPlugin/AlarmController.cpp
        AuthPlugin/AuthController.cpp
        AuthPlugin/OidcController.cpp
        CommandPlugin/CommandController.cpp
        DashboardPlugin/DashboardController.cpp
        DevicePlugin/AgentController.cpp
        DevicePlugin/DeviceController.cpp
        DevicePlugin/NodeGroupController.cpp
        DevicePlugin/NodeJson.cpp
        FirmwarePlugin/FirmwareController.cpp
        MonitorPlugin/MonitorController.cpp
        OpsPlugin/OpsController.cpp
        TelemetryPlugin/TelemetryController.cpp
        SystemPlugin/SystemController.cpp
        SystemPlugin/TenantController.cpp)
add_library(EdgePulse::Plugins ALIAS edgepulse-plugins)

target_include_directories(edgepulse-plugins PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-plugins
        PUBLIC EdgePulse::Core
               AsynGyanis::Net
               AsynGyanis::Core
               AsynGyanis::Base)
edgepulse_enable_warnings(edgepulse-plugins)
