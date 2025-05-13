# tsx plugin
# find_package(OpenMP REQUIRED)

set(tsx_srcs
  ErasureCodePluginTsx.cc
  ErasureCodeTsx.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsx SHARED ${tsx_srcs})
target_compile_options(ec_tsx PRIVATE -mavx2 -O3)
set_target_properties(ec_tsx PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsx json_spirit)
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
target_link_libraries(ec_tsxse json_spirit)
install(TARGETS ec_tsxse DESTINATION ${erasure_plugin_dir})