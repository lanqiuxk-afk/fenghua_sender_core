cmake_minimum_required(VERSION 3.16)
project(fenghua_sender LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
#  FFmpeg 寮€鍙戝寘 (鎶曞睆瑙ｇ爜鐢?銆傚彧闇€瑕佸ご鏂囦欢 + 涓や釜瀵煎叆搴? 杩愯鏃剁敤 DLL銆?#  杩欓噷鎸囧悜鏈満鐜版垚鐨?SDK; 鎹㈡満鍣ㄦ椂鏀硅繖涓€琛屾垨鐢?-DFFMPEG_DIR=... 瑕嗙洊銆?# ---------------------------------------------------------------------------
if(NOT DEFINED FFMPEG_DIR)
    set(FFMPEG_DIR "D:/fenghua_build/ffmpeg_dev")
endif()

set(FFMPEG_OK FALSE)
if(EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h"
   AND EXISTS "${FFMPEG_DIR}/lib/avcodec-61.lib"
   AND EXISTS "${FFMPEG_DIR}/lib/avutil-59.lib")
    set(FFMPEG_OK TRUE)
    message(STATUS "FFmpeg SDK found at ${FFMPEG_DIR} -> 鎶曞睆鍔熻兘鍚敤")
else()
    message(WARNING "FFmpeg SDK not found at ${FFMPEG_DIR} -> 鍙紪璇戦敭榧犲姛鑳?(鎶曞睆涓嶅彲鐢?")
endif()

set(SENDER_SOURCES
    src/main.cpp
    src/input_hook.cpp
)

if(FFMPEG_OK)
    list(APPEND SENDER_SOURCES src/video_player.cpp)
endif()

add_executable(fenghua_sender ${SENDER_SOURCES})

target_include_directories(fenghua_sender PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

if(FFMPEG_OK)
    target_include_directories(fenghua_sender PRIVATE "${FFMPEG_DIR}/include")
    target_compile_definitions(fenghua_sender PRIVATE FH_HAVE_FFMPEG=1)
    target_link_libraries(fenghua_sender PRIVATE
        "${FFMPEG_DIR}/lib/avcodec-61.lib"
        "${FFMPEG_DIR}/lib/avutil-59.lib"
    )
endif()

target_link_libraries(fenghua_sender PRIVATE ws2_32 user32)

if(MSVC)
    target_compile_options(fenghua_sender PRIVATE /utf-8 /O2 /W3)
    set_target_properties(fenghua_sender PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()
