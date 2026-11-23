cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{VITASDK})
  message(FATAL_ERROR "VITASDK environment variable is not set. Please set VITASDK to your VitaSDK path.")
endif()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK Toolchain")

project(OutRun2006-Vita LANGUAGES C CXX ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "OutRun 2006 C2C")
set(VITA_TITLEID  "OR2006C2C")
set(VITA_VERSION  "01.00")

option(OR2_VITA_HEAVY_PROFILE "Enable per-draw render_bottleneck and model_polygons logs" OFF)

add_definitions(
    -D__VITA__
    -D__psp2__
    -DOR2_VITA=1
    -DOR2_X87_FAST=1
    -DOR2_HAVE_VORBIS=1
    -DOR2_VITA_GL=1
)
if(OR2_VITA_HEAVY_PROFILE)
    add_definitions(-DOR2_VITA_HEAVY_PROFILE=1)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -mcpu=cortex-a9 -mfpu=vfpv3-d16 -mfloat-abi=hard -fno-tree-vectorize -ffast-math -fno-finite-math-only -DNDEBUG -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -mcpu=cortex-a9 -mfpu=vfpv3-d16 -mfloat-abi=hard -fno-tree-vectorize -ffast-math -fno-finite-math-only -fno-math-errno -fno-trapping-math -DNDEBUG -ffunction-sections -fdata-sections -fno-rtti -fexceptions -Wno-stringop-overflow")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections -Wl,--defsym=__sce_headroom=0x10000")

get_filename_component(OR2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

include_directories(
    .
    source
    "${OR2_ROOT}/src"
    "$ENV{VITASDK}/arm-vita-eabi/include"
    "$ENV{VITASDK}/arm-vita-eabi/include/SDL2"
)

file(GLOB OR2_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${OR2_ROOT}/src/driving/*.cpp"
    "${OR2_ROOT}/src/platform/*.cpp"
    "${OR2_ROOT}/src/system/*.cpp"
    "${OR2_ROOT}/src/runtime/*.cpp"
    "${OR2_ROOT}/src/enhancements/*.cpp"
)
list(FILTER OR2_ENGINE_SOURCES EXCLUDE REGEX ".*bulk_tr\\.cpp$")

set(VITA_SOURCES
    source/main.cpp
    source/platform_api.cpp
    source/renderer.cpp
    source/sdl_backend.cpp
    source/vita_gl_display.cpp
    source/vita_gl_d3d9.cpp
    source/audio.cpp
    source/setup_display.cpp
    source/enhancements_vita.cpp
    source/bulk_vita_tr.cpp
    source/vita_profiler.cpp
    source/vita_job_system.cpp
    source/vita_render_thread.cpp
    "${OR2_ROOT}/src/input/dual_sense.cpp"
    "${OR2_ROOT}/src/input/runtime_adapter.cpp"
    "${OR2_ROOT}/src/asset_views.cpp"
    "${OR2_ROOT}/src/pc_pmt.cpp"
    "${OR2_ROOT}/src/ports/pecompact_memory.cpp"
)

add_executable(outrun2006_vita
    ${VITA_SOURCES}
    ${OR2_ENGINE_SOURCES}
)

target_link_libraries(outrun2006_vita
    SDL2
    vorbisfile
    vorbis
    ogg
    vitaGL
    vitashark
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    mathneon
    taihen_stub
    SceGxm_stub
    SceDisplay_stub
    SceCtrl_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceAudio_stub
    SceAudioIn_stub
    SceSysmodule_stub
    SceIofilemgr_stub
    SceCommonDialog_stub
    SceTouch_stub
    SceHid_stub
    SceMotion_stub
    ScePower_stub
    SceProcessmgr_stub
    SceRtc_stub
    SceNet_stub
    SceNetCtl_stub
    z
    m
    -Wl,--whole-archive
    pthread
    -Wl,--no-whole-archive
    -Wl,-u,pthread_cancel
)

set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
vita_create_self(eboot.bin outrun2006_vita UNSAFE)
vita_create_vpk(OutRun2006.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
