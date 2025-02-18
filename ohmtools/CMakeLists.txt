
include(GenerateExportHeader)

find_package(GLM)

configure_file(OhmToolsConfig.in.h "${CMAKE_CURRENT_BINARY_DIR}/ohmtools/OhmToolsConfig.h")

set(SOURCES
  OhmCloud.cpp
  OhmCloud.h
  OhmGen.cpp
  OhmGen.h
  OhmToolsConfig.in.h
)

set(PUBLIC_HEADERS
  OhmCloud.h
  OhmGen.h
  "${CMAKE_CURRENT_BINARY_DIR}/ohmtools/OhmToolsConfig.h"
  "${CMAKE_CURRENT_BINARY_DIR}/ohmtools/OhmToolsExport.h"
)

add_library(ohmtools ${SOURCES})
clang_tidy_target(ohmtools)

generate_export_header(ohmtools
      EXPORT_MACRO_NAME ohmtools_API
      EXPORT_FILE_NAME ohmtools/OhmToolsExport.h
      STATIC_DEFINE OHMTOOLS_STATIC)

target_link_libraries(ohmtools PUBLIC ohm ohmheightmap ohmutil)

target_include_directories(ohmtools
  PUBLIC
    $<INSTALL_INTERFACE:${OHM_PREFIX_INCLUDE}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/ohmtools>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
)

target_include_directories(ohmtools SYSTEM
  PUBLIC
    "${GLM_INCLUDE_DIR}"
)

install(TARGETS ohmtools EXPORT ${CMAKE_PROJECT_NAME}-config-targets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION ${OHM_PREFIX_INCLUDE}/ohmtools
)

install(FILES ${PUBLIC_HEADERS} DESTINATION ${OHM_PREFIX_INCLUDE}/ohmtools)

source_group("source" REGULAR_EXPRESSION ".*$")
# Needs CMake 3.8+:
# source_group(TREE "${CMAKE_CURRENT_LIST_DIR}" PREFIX source FILES ${SOURCES})
