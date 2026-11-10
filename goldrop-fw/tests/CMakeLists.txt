cmake_minimum_required(VERSION 3.16)
project(goldrop_host_tests LANGUAGES C CXX)
enable_testing()

add_executable(goldrop_tests
    test_main.cpp
    test_sensors.cpp
    test_network.cpp
    test_server.cpp
    test_ota.cpp
    test_journal.cpp
    test_time.cpp
    ../components/provisioning/wifi_provisioner.cpp
    ../components/storage/wifi_config_codec.cpp
    ../components/storage/server_config_codec.cpp
    ../components/storage/ota_config_codec.cpp
    ../components/storage/flash_journal.cpp
    ../components/ota/ota_manifest.cpp
    ../components/sensors/sensor_service.cpp
    ../components/sensors/fake_sensors.cpp
    ../components/sensors/sensor_protocol.cpp
    ../components/sensors/ds18b20.cpp
    ../components/telemetry/telemetry_service.cpp
    ../components/telemetry/serialization.cpp)
target_sources(goldrop_tests PRIVATE ../components/network/backend_ack.cpp)
target_compile_features(goldrop_tests PRIVATE cxx_std_17)
target_compile_options(goldrop_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(goldrop_tests PRIVATE
    ../components/model/include
    ../components/sensors/include
    ../components/telemetry/include
    ../components/storage/include
    ../components/network/include
    ../components/ota/include)
target_include_directories(goldrop_tests PRIVATE ../components/provisioning/include)
set(CJSON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__cjson/cJSON/cJSON.c")
if(NOT EXISTS "${CJSON_SOURCE}")
    message(FATAL_ERROR "Build firmware once to fetch pinned cJSON for OTA host tests")
endif()
add_library(goldrop_test_cjson STATIC "${CJSON_SOURCE}")
target_include_directories(goldrop_test_cjson PUBLIC ../managed_components/espressif__cjson/cJSON)
target_compile_definitions(goldrop_test_cjson PRIVATE CJSON_NESTING_LIMIT=8)
target_link_libraries(goldrop_tests PRIVATE goldrop_test_cjson)
add_test(NAME goldrop_domain COMMAND goldrop_tests)
add_executable(goldrop_config_tests test_configuration.cpp
    ../components/provisioning/wifi_provisioner.cpp
    ../components/configuration/config_manager.cpp ../components/storage/managed_config_codec.cpp
    ../components/storage/wifi_config_codec.cpp ../components/storage/server_config_codec.cpp
    ../components/management/management_protocol.cpp)
target_compile_features(goldrop_config_tests PRIVATE cxx_std_17)
target_compile_options(goldrop_config_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
target_include_directories(goldrop_config_tests PRIVATE ../components/model/include ../components/storage/include
    ../components/network/include ../components/configuration/include ../components/management/include ../components/provisioning/include ../components/ota/include)
add_test(NAME goldrop_configuration COMMAND goldrop_config_tests)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME goldrop_usb_installer COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_provision_wifi.py)
    add_test(NAME goldrop_ota_package COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_package_ota.py)
    add_test(NAME goldrop_management COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_management.py $<TARGET_FILE:goldrop_config_tests>)
    set_tests_properties(goldrop_management PROPERTIES ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/../build/management-deps")
    add_test(NAME goldrop_fleet_management COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_fleet_management.py $<TARGET_FILE:goldrop_config_tests>)
    set_tests_properties(goldrop_fleet_management PROPERTIES ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/../build/management-deps")
endif()

if(UNIX)
    add_executable(goldrop_wifi_tests test_wifi_manager.cpp wifi_compat/fake_wifi.cpp ../components/network/wifi_manager.cpp)
    target_compile_features(goldrop_wifi_tests PRIVATE cxx_std_17)
    target_compile_options(goldrop_wifi_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_include_directories(goldrop_wifi_tests PRIVATE wifi_compat socket_compat ../components/model/include ../components/network/include)
    add_test(NAME goldrop_wifi_manager COMMAND goldrop_wifi_tests)
    add_executable(goldrop_clock_tests test_system_clock.cpp ../components/network/system_clock.cpp)
    target_compile_features(goldrop_clock_tests PRIVATE cxx_std_17)
    target_compile_options(goldrop_clock_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_include_directories(goldrop_clock_tests PRIVATE socket_compat ../components/model/include ../components/network/include)
    add_test(NAME goldrop_system_clock COMMAND goldrop_clock_tests)
    add_executable(goldrop_boot_tests test_boot_validation.cpp ../components/ota/boot_validation.cpp)
    target_compile_features(goldrop_boot_tests PRIVATE cxx_std_17)
    target_compile_options(goldrop_boot_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_include_directories(goldrop_boot_tests PRIVATE socket_compat ../components/model/include ../components/ota/include)
    add_test(NAME goldrop_boot_validation COMMAND goldrop_boot_tests)
    add_executable(goldrop_tcp_socket_tests test_tcp_socket.cpp
        socket_compat/fake_dns.cpp ../components/network/async_dns.cpp
        ../components/network/tcp_transport.cpp ../components/network/backend_ack.cpp ../components/telemetry/serialization.cpp
        ../components/telemetry/telemetry_service.cpp)
    target_compile_features(goldrop_tcp_socket_tests PRIVATE cxx_std_17)
    target_compile_options(goldrop_tcp_socket_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_include_directories(goldrop_tcp_socket_tests PRIVATE socket_compat
        ../components/model/include ../components/network/include ../components/telemetry/include)
    add_test(NAME goldrop_tcp_socket COMMAND goldrop_tcp_socket_tests)
    if(Python3_Interpreter_FOUND)
        add_test(NAME goldrop_measurement_backend COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_measurement_backend.py $<TARGET_FILE:goldrop_tcp_socket_tests>)
    endif()
    add_executable(goldrop_dns_tests test_async_dns.cpp socket_compat/fake_dns.cpp ../components/network/async_dns.cpp)
    target_compile_features(goldrop_dns_tests PRIVATE cxx_std_17)
    target_compile_options(goldrop_dns_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_include_directories(goldrop_dns_tests PRIVATE socket_compat ../components/model/include ../components/network/include)
    add_test(NAME goldrop_async_dns COMMAND goldrop_dns_tests)
endif()

add_executable(goldrop_sampling_store_tests test_sampling_store.cpp
    ../components/storage/nvs_sampling_config_store.cpp)
target_compile_features(goldrop_sampling_store_tests PRIVATE cxx_std_17)
target_compile_options(goldrop_sampling_store_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
target_include_directories(goldrop_sampling_store_tests PRIVATE nvs_compat socket_compat
    ../components/model/include ../components/storage/include)
target_include_directories(goldrop_sampling_store_tests PRIVATE ../components/ota/include)
add_test(NAME goldrop_sampling_store COMMAND goldrop_sampling_store_tests)

if(UNIX AND Python3_Interpreter_FOUND)
    add_test(NAME goldrop_environment COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_environment.py)
    set_tests_properties(goldrop_environment PROPERTIES ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/../build/management-deps")
endif()
