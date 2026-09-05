cmake_minimum_required(VERSION 3.16)

project(m5_sticks3_host_tests LANGUAGES CXX)

add_executable(app_host_tests
    host_tests.cpp
    ../components/app/src/app_manager.cpp
    ../components/app/src/button_event_bus.cpp
    ../components/mini_games/src/mini_games.cpp
)

target_compile_features(app_host_tests PRIVATE cxx_std_20)
target_compile_options(app_host_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(app_host_tests PRIVATE
    stubs
    ../components/app/include
    ../components/mini_games/include
)

add_executable(connectivity_host_tests connectivity_tests.cpp)
target_compile_features(connectivity_host_tests PRIVATE cxx_std_20)
target_compile_options(connectivity_host_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(connectivity_host_tests PRIVATE
    ../components/connectivity/include
)

enable_testing()
add_test(NAME app_host_tests COMMAND app_host_tests)
add_test(NAME connectivity_host_tests COMMAND connectivity_host_tests)
