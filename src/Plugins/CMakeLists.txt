add_library(edgepulse-plugins STATIC
        Alarm/AlarmController.cpp
        Auth/AuthController.cpp
        Auth/MfaController.cpp
        Auth/OidcController.cpp
        Command/CommandController.cpp
        Dashboard/DashboardController.cpp
        Device/AgentController.cpp
        Device/DeviceController.cpp
        Device/EnrollmentController.cpp
        Device/NodeConfigController.cpp
        Device/NodeGroupController.cpp
        Device/NodeJson.cpp
        Firmware/FirmwareController.cpp
        Monitor/MonitorController.cpp
        Ops/OpsController.cpp
        Telemetry/TelemetryController.cpp
        System/SystemController.cpp
        System/TenantController.cpp
        System/ApiTokenController.cpp
        System/NotificationController.cpp)
add_library(EdgePulse::Plugins ALIAS edgepulse-plugins)

target_include_directories(edgepulse-plugins PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-plugins
        PUBLIC EdgePulse::Core
               AsynGyanis::Net
               AsynGyanis::Core
               AsynGyanis::Base)
edgepulse_enable_warnings(edgepulse-plugins)
