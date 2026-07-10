# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Enable C++20 for std::format support
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/third_party/vqf"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-idf-icm42688
        VERSION 1.0.0
        LANGUAGES C CXX
    )



