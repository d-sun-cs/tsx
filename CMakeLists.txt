# tsx plugin
find_path(HIREDIS_INCLUDE_DIR NAMES hiredis/hiredis.h)
find_library(HIREDIS_LIB NAMES hiredis)
if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIB)
  message(FATAL_ERROR "hiredis headers and library are required (install libhiredis-dev or set HIREDIS_INCLUDE_DIR and HIREDIS_LIB)")
endif()
find_package(OpenMP REQUIRED)

set(tsx_srcs
  ErasureCodePluginTsx.cc
  ErasureCodeTsx.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsx SHARED ${tsx_srcs})
target_include_directories(ec_tsx PRIVATE "${HIREDIS_INCLUDE_DIR}")
# target_compile_options(ec_tsx PRIVATE -mavx2)
target_compile_options(ec_tsx PRIVATE -mavx2 -O3)
set_target_properties(ec_tsx PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsx json_spirit ${HIREDIS_LIB} OpenMP::OpenMP_CXX)
install(TARGETS ec_tsx DESTINATION ${erasure_plugin_dir})


# tsxse plugin

set(tsxse_srcs
  ErasureCodePluginTsxse.cc
  ErasureCodeTsxse.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsxse SHARED ${tsxse_srcs})
target_include_directories(ec_tsxse PRIVATE "${HIREDIS_INCLUDE_DIR}")
target_compile_options(ec_tsxse PRIVATE -mavx2 -O3)
set_target_properties(ec_tsxse PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsxse json_spirit ${HIREDIS_LIB})
install(TARGETS ec_tsxse DESTINATION ${erasure_plugin_dir})

# tsxnaive plugin

set(tsxnaive_srcs
  ErasureCodePluginTsxNaive.cc
  ErasureCodeTsxNaive.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsxnaive SHARED ${tsxnaive_srcs})
target_include_directories(ec_tsxnaive PRIVATE "${HIREDIS_INCLUDE_DIR}")
target_compile_options(ec_tsxnaive PRIVATE -mavx2 -O3)
set_target_properties(ec_tsxnaive PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsxnaive json_spirit ${HIREDIS_LIB})
install(TARGETS ec_tsxnaive DESTINATION ${erasure_plugin_dir})

# tsxmap plugin

set(tsxmap_srcs
  ErasureCodePluginTsxMap.cc
  ErasureCodeTsxMap.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsxmap SHARED ${tsxmap_srcs})
target_include_directories(ec_tsxmap PRIVATE "${HIREDIS_INCLUDE_DIR}")
target_compile_options(ec_tsxmap PRIVATE -mavx2 -O3)
set_target_properties(ec_tsxmap PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsxmap json_spirit ${HIREDIS_LIB})
install(TARGETS ec_tsxmap DESTINATION ${erasure_plugin_dir})


# tsxwin plugin

set(tsxwin_srcs
  ErasureCodePluginTsxWin.cc
  ErasureCodeTsxWin.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsxwin SHARED ${tsxwin_srcs})
target_include_directories(ec_tsxwin PRIVATE "${HIREDIS_INCLUDE_DIR}")
target_compile_options(ec_tsxwin PRIVATE -mavx2 -O3)
set_target_properties(ec_tsxwin PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsxwin json_spirit ${HIREDIS_LIB})
install(TARGETS ec_tsxwin DESTINATION ${erasure_plugin_dir})

# tsxmerge plugin

set(tsxmerge_srcs
  ErasureCodePluginTsxMerge.cc
  ErasureCodeTsxMerge.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsxmerge SHARED ${tsxmerge_srcs})
target_include_directories(ec_tsxmerge PRIVATE "${HIREDIS_INCLUDE_DIR}")
target_compile_options(ec_tsxmerge PRIVATE -mavx2 -O3)
set_target_properties(ec_tsxmerge PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsxmerge json_spirit ${HIREDIS_LIB})
install(TARGETS ec_tsxmerge DESTINATION ${erasure_plugin_dir})

# cerasure plugin
option(WITH_TSX_CERASURE "Build the optional Cerasure comparison plugin" OFF)
if(WITH_TSX_CERASURE)
  find_library(CERASURE_LIB isal PATHS /usr/lib REQUIRED)
  set(cerasure_srcs
    ErasureCodePluginCerasure.cc
    ErasureCodeCerasure.cc
  )

  add_library(ec_cerasure SHARED ${cerasure_srcs})
  target_include_directories(ec_cerasure PRIVATE "${HIREDIS_INCLUDE_DIR}")
  target_link_options(ec_cerasure PRIVATE)
  target_link_libraries(ec_cerasure ${CERASURE_LIB})
  set_target_properties(ec_cerasure PROPERTIES
    INSTALL_RPATH "")
  install(TARGETS ec_cerasure DESTINATION ${erasure_plugin_dir})
endif()

# tsx4 plugin
set(tsx4_srcs
  ErasureCodePluginTsx4.cc
  ErasureCodeTsx4.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsx4 SHARED ${tsx4_srcs} )
target_include_directories(ec_tsx4 PRIVATE "${HIREDIS_INCLUDE_DIR}")
target_compile_options(ec_tsx4 PRIVATE -mavx2 -O3)
set_target_properties(ec_tsx4 PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsx4 json_spirit ${HIREDIS_LIB})
install(TARGETS ec_tsx4 DESTINATION ${erasure_plugin_dir})

# tsx2 plugin
set(tsx2_srcs
  ErasureCodePluginTsx2.cc
  ErasureCodeTsx2.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsx2 SHARED ${tsx2_srcs} )
target_include_directories(ec_tsx2 PRIVATE "${HIREDIS_INCLUDE_DIR}")
target_compile_options(ec_tsx2 PRIVATE -mavx2 -O3)
set_target_properties(ec_tsx2 PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsx2 json_spirit ${HIREDIS_LIB})
install(TARGETS ec_tsx2 DESTINATION ${erasure_plugin_dir})
