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

add_executable(test_fts_word_grouping
  test_fts_word_grouping.cpp
  ${PROJECT_SOURCE_DIR}/src/storage/fts/ob_fts_stop_word.cpp
  ${PROJECT_SOURCE_DIR}/src/storage/fts/ob_fts_struct.cpp)
target_include_directories(test_fts_word_grouping PRIVATE
  ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/query/api ${PROJECT_SOURCE_DIR}/src/data_plane/api)
target_compile_options(test_fts_word_grouping PRIVATE -ffunction-sections -fdata-sections -Wno-invalid-offsetof)
target_link_options(test_fts_word_grouping PRIVATE -Wl,--gc-sections)
target_link_libraries(test_fts_word_grouping PRIVATE ob_share_static oblib ob_malloc)
add_test(NAME test_fts_word_grouping COMMAND test_fts_word_grouping)

add_executable(test_fts_docid_compare
  test_fts_docid_compare.cpp
  ${PROJECT_SOURCE_DIR}/src/storage/retrieval/ob_sparse_daat_iter.cpp)
target_include_directories(test_fts_docid_compare PRIVATE
  ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/query/api ${PROJECT_SOURCE_DIR}/src/data_plane/api)
target_compile_options(test_fts_docid_compare PRIVATE -ffunction-sections -fdata-sections -Wno-invalid-offsetof)
target_link_options(test_fts_docid_compare PRIVATE -Wl,--gc-sections)
target_link_libraries(test_fts_docid_compare PRIVATE ob_share_static oblib ob_malloc)
add_test(NAME test_fts_docid_compare COMMAND test_fts_docid_compare)
