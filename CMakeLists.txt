# tsx plugin

set(tsx_srcs
  ErasureCodePluginTsx.cc
  ErasureCodeTsx.cc
  $<TARGET_OBJECTS:erasure_code_objs>
  $<TARGET_OBJECTS:crush_objs>
  ${CMAKE_SOURCE_DIR}/src/common/str_map.cc
)   

add_library(ec_tsx SHARED ${tsx_srcs})
set_target_properties(ec_tsx PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_tsx json_spirit)
install(TARGETS ec_tsx DESTINATION ${erasure_plugin_dir})
