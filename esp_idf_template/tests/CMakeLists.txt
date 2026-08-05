cmake_minimum_required(VERSION 3.16)

project(esp_idf_template_host_tests LANGUAGES CXX)

add_executable(app_host_tests
    host_tests.cpp
    ../components/app/src/app_manager.cpp
)

target_compile_features(app_host_tests PRIVATE cxx_std_20)
target_compile_options(app_host_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(app_host_tests PRIVATE
    ../components/app/include
)

enable_testing()
add_test(NAME app_host_tests COMMAND app_host_tests)
