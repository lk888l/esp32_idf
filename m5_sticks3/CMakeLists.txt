cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reuse the audited VQF implementation from the requested reference project.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../esp-idf-icm42688/components/third_party/vqf"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5_sticks3
        VERSION 1.0.0
        LANGUAGES C CXX)

