cmake_minimum_required(VERSION 3.24)

project(LabDebugger VERSION 0.6.0 LANGUAGES CXX)

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
    src/core/src/agent_protocol.cpp
    src/core/src/checksum.cpp
    src/core/src/csv_stream_parser.cpp
    src/core/src/frame_stream_parser.cpp
    src/core/src/logger.cpp
    src/core/src/mock_data_source.cpp
    src/core/src/processing_pipeline.cpp
    src/core/src/protocol_decoder.cpp
    src/core/src/protocol_definition.cpp
    src/core/src/protocol_json_loader.cpp
    src/core/src/raw_log_reader.cpp
    src/core/src/raw_log_recorder.cpp
    src/core/src/replay_source.cpp
    src/core/src/remote_agent_session.cpp
    src/core/src/session_recorder.cpp
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
    find_package(Qt6 6.5 QUIET COMPONENTS Widgets SerialPort Network)
    if(Qt6_FOUND)
        qt_standard_project_setup()
        get_target_property(LAB_DEBUGGER_QT_CORE_DLL Qt6::Core IMPORTED_LOCATION_RELEASE)
        if(NOT LAB_DEBUGGER_QT_CORE_DLL)
            get_target_property(LAB_DEBUGGER_QT_CORE_DLL Qt6::Core IMPORTED_LOCATION)
        endif()
        get_filename_component(LAB_DEBUGGER_QT_BIN_DIR
            "${LAB_DEBUGGER_QT_CORE_DLL}" DIRECTORY
        )

        add_library(lab_serial_adapter STATIC
            src/adapters/serial/src/serial_source.cpp
        )
        target_include_directories(lab_serial_adapter PUBLIC
            src/adapters/serial/include
        )
        target_link_libraries(lab_serial_adapter
            PUBLIC lab_core Qt6::Core Qt6::SerialPort
        )

        add_library(lab_network_adapter STATIC
            src/adapters/network/src/network_source.cpp
        )
        target_include_directories(lab_network_adapter PUBLIC
            src/adapters/network/include
        )
        target_link_libraries(lab_network_adapter
            PUBLIC lab_core Qt6::Core Qt6::Network
        )

        add_library(lab_remote_agent_adapter STATIC
            src/adapters/remote_agent/src/remote_agent_source.cpp
        )
        target_include_directories(lab_remote_agent_adapter PUBLIC
            src/adapters/remote_agent/include
        )
        target_link_libraries(lab_remote_agent_adapter
            PUBLIC lab_core Qt6::Core Qt6::Network
        )

        add_library(lab_app_ui STATIC
            src/app/serial_session.cpp
            src/app/serial_session.hpp
            src/ui/main_window.cpp
            src/ui/main_window.hpp
            src/ui/network_panel.cpp
            src/ui/network_panel.hpp
            src/ui/remote_agent_panel.cpp
            src/ui/remote_agent_panel.hpp
            src/ui/plot_widget.cpp
            src/ui/plot_widget.hpp
            src/ui/protocol_widget.cpp
            src/ui/protocol_widget.hpp
            src/ui/replay_widget.cpp
            src/ui/replay_widget.hpp
            src/ui/send_panel.cpp
            src/ui/send_panel.hpp
            src/ui/serial_panel.cpp
            src/ui/serial_panel.hpp
            src/ui/terminal_widget.cpp
            src/ui/terminal_widget.hpp
        )
        target_include_directories(lab_app_ui PUBLIC src)
        target_link_libraries(lab_app_ui
            PUBLIC lab_core lab_serial_adapter lab_network_adapter
                   lab_remote_agent_adapter Qt6::Widgets
        )

        qt_add_executable(lab_debugger WIN32 src/app/main.cpp)
        target_link_libraries(lab_debugger PRIVATE lab_app_ui)
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
            "Qt 6 Widgets + SerialPort + Network were not found. "
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
    add_executable(lab_agent_protocol_tests tests/test_agent_protocol.cpp)
    target_link_libraries(lab_agent_protocol_tests PRIVATE lab_core)
    add_test(NAME lab_agent_protocol_tests COMMAND lab_agent_protocol_tests)
    add_executable(lab_agent_server_session_tests tests/test_agent_server_session.cpp)
    target_link_libraries(lab_agent_server_session_tests PRIVATE lab_core)
    add_test(NAME lab_agent_server_session_tests COMMAND lab_agent_server_session_tests)
    if(UNIX)
        add_executable(lab_agent_tcp_server_tests
            tests/test_agent_tcp_server.cpp
            agent/ros2/lab_debug_agent/src/tcp_server.cpp
        )
        target_include_directories(lab_agent_tcp_server_tests PRIVATE
            agent/ros2/lab_debug_agent/include
        )
        target_link_libraries(lab_agent_tcp_server_tests PRIVATE lab_core)
        target_compile_options(lab_agent_tcp_server_tests PRIVATE
            -Wall -Wextra -Wpedantic
        )
        add_test(NAME lab_agent_tcp_server_tests COMMAND lab_agent_tcp_server_tests)
        set_tests_properties(lab_agent_tcp_server_tests PROPERTIES TIMEOUT 20)
    endif()
    if(TARGET lab_network_adapter)
        add_executable(lab_network_tests tests/test_network.cpp)
        target_link_libraries(lab_network_tests PRIVATE lab_network_adapter Qt6::Core Qt6::Network)
        add_test(NAME lab_network_tests COMMAND lab_network_tests)
        set_tests_properties(lab_network_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${LAB_DEBUGGER_QT_BIN_DIR}"
        )
    endif()
    if(TARGET lab_remote_agent_adapter)
        add_executable(lab_remote_agent_tests tests/test_remote_agent.cpp)
        target_link_libraries(lab_remote_agent_tests
            PRIVATE lab_remote_agent_adapter Qt6::Core Qt6::Network
        )
        add_test(NAME lab_remote_agent_tests COMMAND lab_remote_agent_tests)
        set_tests_properties(lab_remote_agent_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${LAB_DEBUGGER_QT_BIN_DIR}"
            TIMEOUT 15
        )
    endif()
    if(TARGET lab_app_ui)
        add_executable(lab_gui_smoke_tests tests/test_gui_smoke.cpp)
        target_link_libraries(lab_gui_smoke_tests PRIVATE lab_app_ui)
        add_test(NAME lab_gui_smoke_tests COMMAND lab_gui_smoke_tests)
        set_tests_properties(lab_gui_smoke_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${LAB_DEBUGGER_QT_BIN_DIR}"
            TIMEOUT 15
        )
    endif()
endif()
