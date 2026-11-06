cmake_minimum_required(VERSION 3.18)
project(listenica_host C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The same Mbed TLS and cJSON as the firmware: taken from an ESP-IDF checkout when there is one
# (IDF_PATH, or ~/esp/esp-idf), else downloaded at the versions ESP-IDF 5.5.1 ships (checksummed).
if(NOT DEFINED IDF_PATH)
  if(DEFINED ENV{IDF_PATH})
    set(IDF_PATH $ENV{IDF_PATH})
  elseif(WIN32)
    set(IDF_PATH "$ENV{USERPROFILE}/esp/esp-idf")
  else()
    set(IDF_PATH "$ENV{HOME}/esp/esp-idf")
  endif()
endif()
file(TO_CMAKE_PATH "${IDF_PATH}" IDF_PATH)

set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
# no Mbed TLS debug module: it would embed the build machine's source paths in released binaries
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/mbedtls_host_config.h" CACHE FILEPATH "" FORCE)
# Windows: the C runtime linked in, so the exe runs without a Visual C++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(EXISTS "${IDF_PATH}/components/mbedtls/mbedtls/CMakeLists.txt")
  set(MBEDTLS_DIR "${IDF_PATH}/components/mbedtls/mbedtls")
  set(CJSON_DIR "${IDF_PATH}/components/json/cJSON")
else()
  include(FetchContent)
  # SOURCE_SUBDIR points nowhere: download only; their own builds aren't added (Mbed TLS is added below)
  FetchContent_Declare(mbedtls_src
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
    URL_HASH SHA256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110
    SOURCE_SUBDIR download-only)
  FetchContent_Declare(cjson_src
    URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
    URL_HASH SHA256=3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5
    SOURCE_SUBDIR download-only)
  FetchContent_MakeAvailable(mbedtls_src cjson_src)
  set(MBEDTLS_DIR "${mbedtls_src_SOURCE_DIR}")
  set(CJSON_DIR "${cjson_src_SOURCE_DIR}")
endif()
add_subdirectory(${MBEDTLS_DIR} ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)

set(CORE ${CMAKE_CURRENT_LIST_DIR}/../core)
file(GLOB CORE_SRC ${CORE}/src/*.cpp)
add_library(listenica_core STATIC
  ${CORE_SRC}
  ${CORE}/third_party/stb_vorbis_impl.c
  ${CORE}/third_party/puff.c
  ${CORE}/third_party/tinyaes/aes.c
  ${CJSON_DIR}/cJSON.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_host.cpp
)
target_include_directories(listenica_core PUBLIC ${CORE}/include ${CORE}/third_party ${CJSON_DIR})
target_link_libraries(listenica_core PUBLIC mbedtls mbedx509 mbedcrypto)
target_compile_definitions(listenica_core PUBLIC LISTENICA_DIAG=1)  # tests keep the level and decode-speed meters
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/listenica_version.cmake)  # from ../VERSION
target_compile_definitions(listenica_core PUBLIC "LISTENICA_VERSION=\"${LISTENICA_VERSION}\""
                                                 "LISTENICA_BUILD_VERSION=\"${LISTENICA_BUILD_VERSION}\"")
find_package(Threads REQUIRED)
target_link_libraries(listenica_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(listenica_core PUBLIC ws2_32 bcrypt crypt32)  # crypt32: Windows root certificates
  target_compile_definitions(listenica_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  target_compile_options(listenica_core PRIVATE /W3 /utf-8)
endif()

# The program users run: speaker output, mDNS discovery and a few desktop helpers per OS.
add_executable(listenica_host main.cpp desktop.cpp)
if(WIN32)
  target_sources(listenica_host PRIVATE speaker_win.cpp lan_discovery_win.cpp)
  target_link_libraries(listenica_host listenica_core winmm dnsapi shell32)  # waveOut; DnsService*; ShellExecute
elseif(APPLE)
  target_sources(listenica_host PRIVATE speaker_mac.cpp lan_discovery_dnssd.cpp)  # dns_sd is in libSystem
  target_link_libraries(listenica_host listenica_core "-framework AudioToolbox" "-framework CoreAudio"
                        "-framework CoreFoundation")
else()  # Linux: apt install libasound2-dev libavahi-compat-libdnssd-dev (runtime: avahi-daemon)
  target_sources(listenica_host PRIVATE speaker_linux.cpp lan_discovery_dnssd.cpp)
  target_link_libraries(listenica_host listenica_core asound dns_sd)
endif()
if(MSVC)
  target_compile_options(listenica_host PRIVATE /utf-8)
endif()

add_executable(unit_tests unit_tests.cpp)
target_link_libraries(unit_tests listenica_core)

add_executable(listenica_bench bench.cpp)  # decode benchmark (host only)
target_link_libraries(listenica_bench listenica_core)
