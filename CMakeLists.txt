# tsx plugin
find_library(HIREDIS_LIB hiredis PATHS /usr/local/lib REQUIRED)
find_package(OpenMP REQUIRED)

set(tsx_srcs
  ErasureCodePluginTsx.cc
  ErasureCodeTsx.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsx SHARED ${tsx_srcs})
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
target_compile_options(ec_tsxmerge PRIVATE -mavx2 -O3)
set_target_properties(ec_tsxmerge PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsxmerge json_spirit ${HIREDIS_LIB})
install(TARGETS ec_tsxmerge DESTINATION ${erasure_plugin_dir})

# cerasure plugin
find_library(CERASURE_LIB isal PATHS /usr/lib REQUIRED)
set(cerasure_srcs
  ErasureCodePluginCerasure.cc
  ErasureCodeCerasure.cc
)   

add_library(ec_cerasure SHARED ${cerasure_srcs} )
target_link_options(ec_cerasure PRIVATE)
target_link_libraries(ec_cerasure ${CERASURE_LIB})
set_target_properties(ec_cerasure PROPERTIES
  INSTALL_RPATH "")
install(TARGETS ec_cerasure DESTINATION ${erasure_plugin_dir})