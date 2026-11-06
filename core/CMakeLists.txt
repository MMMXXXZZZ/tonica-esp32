# listenica core as an ESP-IDF component (the host build has its own CMakeLists
# in ../host and does not use this file).
file(GLOB CORE_SRC ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
idf_component_register(
  SRCS ${CORE_SRC}
       third_party/stb_vorbis_impl.c
       third_party/puff.c
       third_party/tinyaes/aes.c
       port/esp/platform_esp.cpp
  INCLUDE_DIRS include third_party
  REQUIRES mbedtls json esp_timer esp_system pthread lwip
  PRIV_REQUIRES heap freertos
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable)
set_source_files_properties(third_party/stb_vorbis_impl.c PROPERTIES COMPILE_OPTIONS "-O2;-Wno-unused-value;-Wno-maybe-uninitialized;-Wno-misleading-indentation")
