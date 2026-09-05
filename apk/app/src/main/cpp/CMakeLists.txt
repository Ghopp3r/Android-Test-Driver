cmake_minimum_required(VERSION 3.22)
project(mydrivertest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Repo layout: apk/app/src/main/cpp  ->  ../../../..  = apk/  ->  ..  = repo root
set(DRIVER_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(CLIENT_SRC ${DRIVER_REPO_ROOT}/client/src)
set(DRIVER_INCLUDE ${DRIVER_REPO_ROOT}/driver/include)

if(NOT EXISTS ${CLIENT_SRC}/Driver.cpp)
    message(FATAL_ERROR "Cannot find Driver.cpp at ${CLIENT_SRC}. Repo layout has drifted.")
endif()
if(NOT EXISTS ${DRIVER_INCLUDE}/driver/uapi.h)
    message(FATAL_ERROR "Cannot find driver/uapi.h at ${DRIVER_INCLUDE}. Repo layout has drifted.")
endif()

add_library(mydrivertest SHARED
    jni_bridge.cpp
    ${CLIENT_SRC}/Driver.cpp
)

target_include_directories(mydrivertest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CLIENT_SRC}
    ${DRIVER_INCLUDE}
)

target_compile_options(mydrivertest PRIVATE
    -Wall
    -Wno-unused-parameter
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
)

target_link_libraries(mydrivertest
    log
    android
)

target_link_options(mydrivertest PRIVATE
    -Wl,--gc-sections
    -Wl,--build-id=sha1
)
