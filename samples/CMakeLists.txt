# 可执行文件统一输出到 samples/bin：samples/<目标名> 同时是 CMake 的目标目录，同名会让 Linux 链接失败
add_subdirectory(H3Probe)
add_subdirectory(DeviceAgent)
add_subdirectory(FirmwareVerifier)
