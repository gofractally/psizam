# Namespaced compatibility snapshot needed by the engine's existing PZAM ABI.
add_library(psio1 STATIC src/fpconv.c src/from_json.cpp src/chrono.cpp src/emit_wit.cpp src/schema.cpp)
target_include_directories(psio1 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rapidjson/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(psio1 PUBLIC cxx_std_23)
target_link_libraries(psio1 PUBLIC Boost::headers)
if(PSIZAM_ENABLE_EXAMPLES OR PSIZAM_ENABLE_TESTS)
  add_executable(psio_tool tools/psio_tool.cpp)
  target_link_libraries(psio_tool PRIVATE psio1)
endif()
