cmake_minimum_required(VERSION 3.24)

project(LabDebugger VERSION 0.2.0 LANGUAGES CXX)

if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    include(InstallRequiredSystemLibraries)
endif()

option(LAB_DEBUGGER_BUILD_GUI "Build the Qt desktop application" ON)
option(LAB_DEBUGGER_BUILD_TESTS "Build core unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/EHsc /utf-8)
endif()

find_package(Threads REQUIRED)

add_library(lab_core STATIC
    src/core/src/checksum.cpp
    src/core/src/csv_stream_parser.cpp
    src/core/src/frame_stream_parser.cpp
    src/core/src/logger.cpp
    src/core/src/mock_data_source.cpp
    src/core/src/processing_pipeline.cpp
    src/core/src/protocol_decoder.cpp
    src/core/src/protocol_definition.cpp
    src/core/src/protocol_json_loader.cpp
    src/core/src/raw_log_recorder.cpp
    src/core/src/time_series_store.cpp
)
target_include_directories(lab_core PUBLIC src/core/include)
target_link_libraries(lab_core PUBLIC Threads::Threads)
target_compile_features(lab_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(lab_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(lab_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LAB_DEBUGGER_BUILD_GUI)
    find_package(Qt6 6.5 QUIET COMPONENTS Widgets SerialPort)
    if(Qt6_FOUND)
        qt_standard_project_setup()

        add_library(lab_serial_adapter STATIC
            src/adapters/serial/src/serial_source.cpp
        )
        target_include_directories(lab_serial_adapter PUBLIC
            src/adapters/serial/include
        )
        target_link_libraries(lab_serial_adapter
            PUBLIC lab_core Qt6::Core Qt6::SerialPort
        )

        qt_add_executable(lab_debugger WIN32
            src/app/main.cpp
            src/app/serial_session.cpp
            src/app/serial_session.hpp
            src/ui/main_window.cpp
            src/ui/main_window.hpp
            src/ui/plot_widget.cpp
            src/ui/plot_widget.hpp
            src/ui/send_panel.cpp
            src/ui/send_panel.hpp
            src/ui/serial_panel.cpp
            src/ui/serial_panel.hpp
            src/ui/terminal_widget.cpp
            src/ui/terminal_widget.hpp
        )
        target_include_directories(lab_debugger PRIVATE src)
        target_link_libraries(lab_debugger
            PRIVATE lab_core lab_serial_adapter Qt6::Widgets
        )
        set_target_properties(lab_debugger PROPERTIES
            OUTPUT_NAME "LabDebugger"
        )

        install(TARGETS lab_debugger
            RUNTIME DESTINATION bin
            BUNDLE DESTINATION .
        )
        install(FILES docs/RUN_WINDOWS.txt DESTINATION .)
        qt_generate_deploy_app_script(
            TARGET lab_debugger
            OUTPUT_SCRIPT deploy_script
            NO_UNSUPPORTED_PLATFORM_ERROR
        )
        install(SCRIPT ${deploy_script})
    else()
        message(WARNING
            "Qt 6 Widgets + SerialPort were not found. "
            "The portable core and tests will still be built."
        )
    endif()
endif()

if(LAB_DEBUGGER_BUILD_TESTS)
    enable_testing()
    add_executable(lab_core_tests
        tests/test_core.cpp
        tests/test_protocol.cpp
    )
    target_link_libraries(lab_core_tests PRIVATE lab_core)
    add_test(NAME lab_core_tests COMMAND lab_core_tests)
endif()
