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

add_executable(ble_bond_store_tests ble_bond_store_tests.cpp)
target_compile_features(ble_bond_store_tests PRIVATE cxx_std_20)
target_compile_options(ble_bond_store_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(ble_bond_store_tests PRIVATE
    ble_bond_stubs
    ../components/connectivity/src
)
add_test(NAME ble_bond_store_tests COMMAND ble_bond_store_tests)

find_package(Threads REQUIRED)
add_executable(bsp_power_tests
    bsp_power_tests.cpp
    ../components/bsp/src/bsp_board.cpp
)
target_compile_features(bsp_power_tests PRIVATE cxx_std_20)
target_compile_options(bsp_power_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(bsp_power_tests PRIVATE bsp_stubs ../components/bsp/include)
target_link_libraries(bsp_power_tests PRIVATE Threads::Threads)
add_test(NAME bsp_power_tests COMMAND bsp_power_tests)

add_executable(audio_dsp_tests audio_dsp_tests.cpp)
target_compile_features(audio_dsp_tests PRIVATE cxx_std_20)
target_compile_options(audio_dsp_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(audio_dsp_tests PRIVATE ../components/audio/include)
add_test(NAME audio_dsp_tests COMMAND audio_dsp_tests)

add_executable(infrared_protocol_tests
    infrared_protocol_tests.cpp
    ../components/infrared/src/infrared_protocol.cpp
)
target_compile_features(infrared_protocol_tests PRIVATE cxx_std_20)
target_compile_options(infrared_protocol_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(infrared_protocol_tests PRIVATE ../components/infrared/include)
add_test(NAME infrared_protocol_tests COMMAND infrared_protocol_tests)

option(M5_HOST_SANITIZERS "Enable host AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(M5_HOST_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(test_target app_host_tests connectivity_host_tests ble_bond_store_tests
                        bsp_power_tests audio_dsp_tests infrared_protocol_tests)
        target_compile_options(${test_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${test_target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
