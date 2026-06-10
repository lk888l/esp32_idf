# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Generate compile_commands.json for editor IntelliSense and tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(template_project 
    VERSION 1.0.0
    LANGUAGES C CXX
    DESCRIPTION "Professional ESP-IDF Project Template"
    HOMEPAGE_URL "https://your-url.com"
)

# Set C/C++ standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set C++ specific options
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -fno-rtti")

# Set compilation flags for better code quality
add_compile_options(
    -Wall
    -Wextra

    -Wwrite-strings
    -Wuninitialized
    -Winit-self
    -Wduplicated-branches
    -Wduplicated-cond
    
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Wshadow>
)

# Project configuration
set(PROJECT_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/config")
set(PROJECT_DOCS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/docs")
set(PROJECT_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools")

# Define compile-time configuration variables
add_definitions(
    -DPROJECT_NAME="${PROJECT_NAME}"
    -DPROJECT_VERSION="${PROJECT_VERSION}"
    -DBUILD_TIMESTAMP="${CMAKE_CURRENT_TIMESTAMP}"
)

# Include the main application
include_directories(BEFORE PRIVATE ${PROJECT_BINARY_DIR}/include)
