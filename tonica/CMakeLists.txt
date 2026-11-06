cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../core)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Published images must not carry this machine's paths (asserts embed __FILE__, which holds the
# home folder name): map the repo and IDF roots to neutral prefixes.
get_filename_component(TONICA_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(TO_CMAKE_PATH "$ENV{IDF_PATH}" TONICA_IDF_ROOT)
idf_build_set_property(COMPILE_OPTIONS "-ffile-prefix-map=${TONICA_REPO_ROOT}=/listenica" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-ffile-prefix-map=${TONICA_IDF_ROOT}=/IDF" APPEND)
# The version from ../VERSION: the image header (PROJECT_VER, read by OTA and esptool) and the code.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/listenica_version.cmake)
set(PROJECT_VER "${LISTENICA_BUILD_VERSION}")
idf_build_set_property(COMPILE_DEFINITIONS "LISTENICA_VERSION=\"${LISTENICA_VERSION}\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LISTENICA_BUILD_VERSION=\"${LISTENICA_BUILD_VERSION}\"" APPEND)
project(tonica_esp32)
