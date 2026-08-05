cmake_minimum_required(VERSION 3.20)
project(canopen_core_tests LANGUAGES CXX)

enable_testing()
add_executable(canopen_core_tests
    test_canopen.cpp
    ../components/canopen/src/object_dictionary.cpp
    ../components/canopen/src/sdo_server.cpp
    ../components/canopen/src/pdo.cpp
    ../components/canopen/src/node.cpp
    ../components/canopen/src/standard_profile.cpp
)
target_compile_features(canopen_core_tests PRIVATE cxx_std_20)
target_compile_options(canopen_core_tests PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(canopen_core_tests PRIVATE
    ../components/can_transport/include
    ../components/canopen/include
)
add_test(NAME canopen_core COMMAND canopen_core_tests)

