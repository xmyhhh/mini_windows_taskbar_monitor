cmake_minimum_required(VERSION 3.20)

project(minimal_taskbar_monitor LANGUAGES CXX)

add_executable(minimal_taskbar_monitor WIN32
    src/main.cpp
    src/app_config.cpp
    src/system_metrics.cpp
    src/taskbar_embedder.cpp
)

target_compile_features(minimal_taskbar_monitor PRIVATE cxx_std_20)

target_compile_definitions(minimal_taskbar_monitor PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
)

target_link_libraries(minimal_taskbar_monitor PRIVATE
    pdh
    iphlpapi
    shell32
    advapi32
)
