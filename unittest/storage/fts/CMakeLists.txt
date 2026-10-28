add_executable(test_fts_hot_path
  test_fts_hot_path.cpp
  ${PROJECT_SOURCE_DIR}/src/storage/fts/ob_beng_ft_parser.cpp
  ${PROJECT_SOURCE_DIR}/src/storage/fts/utils/unicode_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/share/text_analysis/ob_token_stream.cpp
  ${PROJECT_SOURCE_DIR}/src/share/text_analysis/ob_text_analyzer.cpp)
target_include_directories(test_fts_hot_path PRIVATE
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/src/query/api
  ${PROJECT_SOURCE_DIR}/src/data_plane/api)
target_compile_options(test_fts_hot_path PRIVATE -ffunction-sections -fdata-sections -Wno-invalid-offsetof)
target_link_options(test_fts_hot_path PRIVATE -Wl,--gc-sections)
target_link_libraries(test_fts_hot_path PRIVATE oblib ob_malloc)
add_test(NAME test_fts_hot_path COMMAND test_fts_hot_path)
