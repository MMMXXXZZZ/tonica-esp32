# The Listenica version. VERSION at the repo root is the only place it is set; the firmware
# (tonica/CMakeLists.txt) and the host (host/CMakeLists.txt) both include this file.
#
#   LISTENICA_VERSION        "0.1.0"            what Spotify and other clients are told
#   LISTENICA_BUILD_VERSION  "0.1.0"            a release build (-DLISTENICA_RELEASE=ON)
#                            "0.1.0-dev+1a2b3c4" any other build (commit as of the last CMake configure)
#
# The build version goes into logs, mDNS and the ESP-IDF image header (max 31 characters).

get_filename_component(_lv_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(STRINGS "${_lv_root}/VERSION" LISTENICA_VERSION LIMIT_COUNT 1)
string(STRIP "${LISTENICA_VERSION}" LISTENICA_VERSION)
if(NOT LISTENICA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must hold MAJOR.MINOR.PATCH, found '${LISTENICA_VERSION}'")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_lv_root}/VERSION")

option(LISTENICA_RELEASE "Release build: the version without the -dev suffix" OFF)
if(LISTENICA_RELEASE)
  set(LISTENICA_BUILD_VERSION "${LISTENICA_VERSION}")
else()
  set(_lv_sha "")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                    WORKING_DIRECTORY "${_lv_root}" RESULT_VARIABLE _lv_rc
                    OUTPUT_VARIABLE _lv_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _lv_rc EQUAL 0)
      set(_lv_sha "")  # no commits yet
    endif()
  endif()
  if(_lv_sha)
    set(LISTENICA_BUILD_VERSION "${LISTENICA_VERSION}-dev+${_lv_sha}")
  else()
    set(LISTENICA_BUILD_VERSION "${LISTENICA_VERSION}-dev")
  endif()
endif()
message(STATUS "Listenica version ${LISTENICA_BUILD_VERSION}")
