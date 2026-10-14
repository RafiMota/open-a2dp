cmake_minimum_required(VERSION 3.21)
project(OpenA2DP VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OA2DP_BUILD_TESTS "Build unit tests" ON)
option(OA2DP_BUILD_DRIVERS "Build kernel drivers (needs the WDK and FindWDK)" OFF)

set(OA2DP_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# --- portable protocol core ------------------------------------------------
add_library(oa2dp_core STATIC
    core/src/a2dp_codecs.c
    core/src/avdtp.c
    core/src/media.c
    core/src/pcm_ring.c
    core/src/settings.c)
target_include_directories(oa2dp_core PUBLIC core/include)
if(MSVC)
    target_compile_options(oa2dp_core PRIVATE /W4)
else()
    target_compile_options(oa2dp_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

if(OA2DP_BUILD_TESTS)
    enable_testing()
    add_executable(oa2dp_tests
        core/tests/test_main.c core/tests/test_avdtp.c core/tests/test_codecs.c
        core/tests/test_media.c core/tests/test_ring.c core/tests/test_settings.c)
    target_link_libraries(oa2dp_tests PRIVATE oa2dp_core)
    add_test(NAME core COMMAND oa2dp_tests)
endif()

if(NOT WIN32)
    return()  # everything below is Windows-only
endif()

# --- codec adapters ---------------------------------------------------------
add_library(oa2dp_codecs STATIC codecs/src/encoder.c codecs/src/enc_sbc.c
    codecs/src/enc_aptx.c codecs/src/enc_ldac.c codecs/src/enc_aac_mf.cpp)
target_include_directories(oa2dp_codecs PUBLIC codecs/include)
target_link_libraries(oa2dp_codecs PUBLIC oa2dp_core)

# AAC: Media Foundation, part of Windows.
target_compile_definitions(oa2dp_codecs PUBLIC OA2DP_WITH_MFAAC)
target_link_libraries(oa2dp_codecs PUBLIC mfplat mfuuid ole32)

# SBC + FastStream: libsbc (LGPL-2.1+), built as a DLL to keep it replaceable.
if(EXISTS ${OA2DP_THIRD_PARTY}/sbc/sbc/sbc.c)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sbc-config/config.h "#include <stdint.h>\n")
    add_library(sbc SHARED ${OA2DP_THIRD_PARTY}/sbc/sbc/sbc.c
        ${OA2DP_THIRD_PARTY}/sbc/sbc/sbc_primitives.c)
    target_include_directories(sbc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/sbc-config
        PUBLIC ${OA2DP_THIRD_PARTY}/sbc)
    set_target_properties(sbc PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(MSVC)  # libsbc's public header uses POSIX ssize_t
        target_compile_definitions(sbc PUBLIC ssize_t=intptr_t)
    endif()
    target_compile_definitions(oa2dp_codecs PUBLIC OA2DP_WITH_LIBSBC)
    target_link_libraries(oa2dp_codecs PUBLIC sbc)
endif()

# aptX / aptX HD / aptX LL: libopenaptx (GPL-3.0+, see THIRD_PARTY.md).
if(EXISTS ${OA2DP_THIRD_PARTY}/libopenaptx/openaptx.c)
    add_library(openaptx SHARED ${OA2DP_THIRD_PARTY}/libopenaptx/openaptx.c)
    target_include_directories(openaptx PUBLIC ${OA2DP_THIRD_PARTY}/libopenaptx)
    set_target_properties(openaptx PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_definitions(oa2dp_codecs PUBLIC OA2DP_WITH_OPENAPTX)
    target_link_libraries(oa2dp_codecs PUBLIC openaptx)
endif()

# LDAC + adaptive bitrate: AOSP libldac (Apache-2.0).
if(EXISTS ${OA2DP_THIRD_PARTY}/libldac/src/ldacBT.c)
    add_library(ldac SHARED ${OA2DP_THIRD_PARTY}/libldac/src/ldaclib.c
        ${OA2DP_THIRD_PARTY}/libldac/src/ldacBT.c
        ${OA2DP_THIRD_PARTY}/libldac/abr/src/ldacBT_abr.c)
    target_include_directories(ldac PUBLIC ${OA2DP_THIRD_PARTY}/libldac/inc
        ${OA2DP_THIRD_PARTY}/libldac/abr/inc)
    set_target_properties(ldac PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_definitions(oa2dp_codecs PUBLIC OA2DP_WITH_LIBLDAC OA2DP_WITH_LDAC_ABR)
    target_link_libraries(oa2dp_codecs PUBLIC ldac)
endif()

if(OA2DP_BUILD_TESTS)
    add_executable(test_encoders codecs/tests/test_encoders.c)
    target_link_libraries(test_encoders PRIVATE oa2dp_codecs)
    add_test(NAME encoders COMMAND test_encoders)
endif()

# --- service ----------------------------------------------------------------
add_executable(oa2dpsvc
    service/src/main.cpp service/src/session.cpp service/src/driver_io.cpp
    service/src/settings_store.cpp service/src/log.cpp)
target_include_directories(oa2dpsvc PRIVATE driver/common)
target_compile_definitions(oa2dpsvc PRIVATE UNICODE _UNICODE)
target_link_libraries(oa2dpsvc PRIVATE oa2dp_codecs cfgmgr32 bthprops shell32 advapi32)

# --- tools: shared device/settings logic, command line, graphical manager ------
add_library(oa2dp_manage STATIC tools/common/manage.cpp)
target_include_directories(oa2dp_manage PUBLIC tools/common)
target_compile_definitions(oa2dp_manage PUBLIC UNICODE _UNICODE)
target_link_libraries(oa2dp_manage PUBLIC oa2dp_core setupapi bthprops advapi32)

add_executable(oa2dpctl tools/oa2dpctl/oa2dpctl.cpp)
target_link_libraries(oa2dpctl PRIVATE oa2dp_manage)

add_executable(oa2dpgui WIN32 tools/oa2dpgui/oa2dpgui.cpp tools/oa2dpgui/oa2dpgui.rc)
target_link_libraries(oa2dpgui PRIVATE oa2dp_manage comctl32 shell32 user32 gdi32)
if(MSVC)
    # The .rc already embeds tools/oa2dpgui/oa2dpgui.manifest.
    target_link_options(oa2dpgui PRIVATE /MANIFEST:NO)
elseif(MINGW)
    target_link_options(oa2dpgui PRIVATE -municode)
endif()

# --- drivers ------------------------------------------------------------------
if(OA2DP_BUILD_DRIVERS)
    list(APPEND CMAKE_MODULE_PATH ${OA2DP_THIRD_PARTY}/FindWDK/cmake)
    find_package(WDK REQUIRED)
    wdk_add_driver(oa2dp KMDF 1.15 driver/oa2dp/driver.c driver/oa2dp/l2cap.c)

    # Virtual render endpoint: PortCls/WaveRT, derived from SYSVAD (MS-PL).
    # NTDDI 10.0.19041 for ExAllocatePool2; matches the INF decoration.
    wdk_add_driver(oa2dpaudio WINVER 0x0A00 NTDDI_VERSION 0x0A000008
        driver/oa2dpaudio/adapter.cpp
        driver/oa2dpaudio/control.cpp
        driver/oa2dpaudio/minwavert.cpp
        driver/oa2dpaudio/minwavertstream.cpp
        driver/oa2dpaudio/mintopo.cpp
        driver/oa2dpaudio/newdelete.cpp
        driver/oa2dpaudio/ring.c
        core/src/pcm_ring.c)
    target_include_directories(oa2dpaudio PRIVATE core/include driver/common)
    target_compile_definitions(oa2dpaudio PRIVATE PC_IMPLEMENTATION UNICODE _UNICODE)
    target_link_libraries(oa2dpaudio WDK::PORTCLS WDK::STDUNK WDK::LIBCNTPR)
endif()
