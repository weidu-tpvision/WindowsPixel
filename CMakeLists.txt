cmake_minimum_required(VERSION 3.22.1)
project(winvm LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall
    -Wextra
    -O2
    -g
    -D_GNU_SOURCE
)

# Android NDK enables ShadowCallStack (SCS) in some configurations, which
# reserves x18 as the shadow call stack pointer. Our ARM64 JIT steals x18
# while in guest code and saves/restores host x18 across helper calls, so we
# disable SCS instrumentation on C compilation units.
if(ANDROID)
    add_compile_options(-fno-sanitize=shadow-call-stack)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/devices
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libslirp
)

add_compile_definitions(HAVE_SLIRP=1)

set(CORE_SRCS
    core/classify.c
    core/operands.c
    core/translate.c
    core/dispatch.c
    core/mmu.c
    core/memop.c
    core/exception.c
    core/sysreg.c
    core/machine.c
)

set(HOST_ENGINE_SRCS
    host/jit_region.c
    host/enter.S
    host/disk.c
)

set(DEVICE_SRCS
    devices/acpi.c
    devices/fb_display.c
    devices/fdt.c
    devices/fw_cfg.c
    devices/gicv3.c
    devices/hda.c
    devices/nvme.c
    devices/pcie.c
    devices/pflash.c
    devices/pl011.c
    devices/pl031.c
    devices/pl061.c
    devices/timer.c
    devices/virtio.c
    devices/virtio_blk.c
    devices/virtio_input.c
    devices/virtio_net.c
    devices/virtio_snd.c
    devices/winvirt.c
    devices/xhci.c
)

set(VM_HOST_SRCS
    host/winvm.c
    host/net_slirp.c
    host/share.c
    host/hvsparse.c
)

file(GLOB SLIRP_SRCS "third_party/libslirp/*.c")
set_source_files_properties(${SLIRP_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable")
list(APPEND VM_HOST_SRCS ${SLIRP_SRCS})

if(ANDROID)
    list(APPEND DEVICE_SRCS devices/fb_android.c)
    list(APPEND VM_HOST_SRCS host/audio_android.c)
endif()

# CRITICAL: hv_tlb_refill runs while guest SIMD/FP registers v0..v31 are live
# in host hardware registers. Prevent the compiler from touching v0..v31.
set_source_files_properties(core/mmu.c PROPERTIES COMPILE_FLAGS "-mgeneral-regs-only")

add_library(winvm_engine OBJECT ${CORE_SRCS} ${HOST_ENGINE_SRCS})
set_property(TARGET winvm_engine PROPERTY POSITION_INDEPENDENT_CODE ON)

add_library(winvm SHARED
    $<TARGET_OBJECTS:winvm_engine>
    ${DEVICE_SRCS}
    ${VM_HOST_SRCS}
)

add_library(winvm_static STATIC
    $<TARGET_OBJECTS:winvm_engine>
    ${DEVICE_SRCS}
    ${VM_HOST_SRCS}
)
set_target_properties(winvm_static PROPERTIES OUTPUT_NAME winvm)

if(ANDROID)
    find_library(log-lib log)
    find_library(android-lib android)
    find_library(egl-lib EGL)
    find_library(gles3-lib GLESv3)
    find_library(aaudio-lib aaudio)
    target_link_libraries(winvm PRIVATE ${egl-lib} ${gles3-lib} ${android-lib} ${aaudio-lib} ${log-lib} m)
    target_link_libraries(winvm_static INTERFACE ${egl-lib} ${gles3-lib} ${android-lib} ${aaudio-lib} ${log-lib} m)
else()
    target_link_libraries(winvm PRIVATE pthread m)
endif()

option(WINVM_BUILD_TESTS "Build core engine unit tests and tools" ON)
if(WINVM_BUILD_TESTS AND NOT ANDROID)
    add_executable(hvsparse tools/hvsparse_main.c host/hvsparse.c)
    add_executable(winvm_cli host/main_linux.c)
    set_target_properties(winvm_cli PROPERTIES OUTPUT_NAME winvm-cli)
    target_link_libraries(winvm_cli PRIVATE winvm_static pthread m)

    add_executable(test_classify tests/test_classify.c tests/classify_cases.s core/classify.c)
    add_executable(test_jit_region tests/test_jit_region.c host/jit_region.c)
    add_executable(test_operands tests/test_operands.c tests/operand_cases.s core/operands.c)
    add_executable(test_mmu tests/test_mmu.c $<TARGET_OBJECTS:winvm_engine>)
    target_link_libraries(test_mmu PRIVATE pthread)

    foreach(t jit mem exc bench)
        add_executable(test_${t}
            tests/test_${t}.c
            tests/${t}_programs.S
            tests/harness.S
            $<TARGET_OBJECTS:winvm_engine>
        )
        target_include_directories(test_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(test_${t} PRIVATE pthread)
    endforeach()
endif()
