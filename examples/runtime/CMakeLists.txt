add_executable(hello_runtime hello_runtime.cpp)
target_link_libraries(hello_runtime PRIVATE psizam psizam-exec)
target_include_directories(hello_runtime PRIVATE
   ${PSIZAM_SOURCE_DIR}/include)
