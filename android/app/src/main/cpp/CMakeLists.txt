cmake_minimum_required(VERSION 3.22.1)
project(winvm_jni LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(WINVM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
add_subdirectory(${WINVM_ROOT} winvm_build)

add_library(winvm_jni SHARED
    winvm_jni.c
)

target_include_directories(winvm_jni PRIVATE
    ${WINVM_ROOT}/core
    ${WINVM_ROOT}/host
    ${WINVM_ROOT}/devices
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles3-lib GLESv3)
find_library(aaudio-lib aaudio)

target_link_libraries(winvm_jni PRIVATE
    winvm_static
    ${egl-lib}
    ${gles3-lib}
    ${android-lib}
    ${aaudio-lib}
    ${log-lib}
    m
)
