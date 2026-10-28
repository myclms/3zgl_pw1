# A candidate must not accidentally link the baseline's prebuilt bridge. Keep
# the source-built artifact in this build tree with a distinct SONAME so that a
# baseline library on LD_LIBRARY_PATH cannot shadow the candidate implementation.
function(seekdb_add_cuvs_bridge target)
  if(TARGET seekdb_cuvs_bridge_candidate)
    target_link_libraries(${target} PUBLIC seekdb_cuvs_bridge_candidate)
    return()
  endif()
  if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "The AMD hipVS bridge requires its Linux ROCm build environment")
  endif()
  find_program(SEEKDB_HIPCC NAMES hipcc
    HINTS "$ENV{ROCM_ROOT}/bin" /opt/rocm/bin REQUIRED)
  find_path(SEEKDB_HIPVS_INCLUDE_DIR cuvs/core/c_api.h
    HINTS "$ENV{HIPVS_ROOT}/include" /opt/hipvs/include REQUIRED)
  get_filename_component(_hipvs_root "${SEEKDB_HIPVS_INCLUDE_DIR}" DIRECTORY)
  get_filename_component(_rocm_bin "${SEEKDB_HIPCC}" DIRECTORY)
  get_filename_component(_rocm_root "${_rocm_bin}" DIRECTORY)
  find_program(SEEKDB_BASH NAMES bash REQUIRED)
  set(_bridge "${CMAKE_BINARY_DIR}/hipvs/libseekdb_cuvs_bridge_candidate.so")
  add_custom_command(OUTPUT "${_bridge}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "HIPCC=${SEEKDB_HIPCC}" "HIPVS_ROOT=${_hipvs_root}"
      "ROCM_ROOT=${_rocm_root}"
      "CUVS_BRIDGE_SONAME=libseekdb_cuvs_bridge_candidate.so"
      "${SEEKDB_BASH}" "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge/build.sh" "${_bridge}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge/build.sh"
      "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge/seekdb_cuvs_bridge.c"
      "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge/seekdb_cuvs_bridge.h"
    VERBATIM)
  add_custom_target(seekdb_cuvs_bridge_candidate_build DEPENDS "${_bridge}")
  add_library(seekdb_cuvs_bridge_candidate SHARED IMPORTED GLOBAL)
  set_target_properties(seekdb_cuvs_bridge_candidate PROPERTIES
    IMPORTED_LOCATION "${_bridge}"
    IMPORTED_SONAME libseekdb_cuvs_bridge_candidate.so)
  add_dependencies(seekdb_cuvs_bridge_candidate seekdb_cuvs_bridge_candidate_build)
  target_link_libraries(${target} PUBLIC seekdb_cuvs_bridge_candidate)
endfunction()
