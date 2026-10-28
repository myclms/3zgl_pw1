#ifndef MOCK_GPU_H
#define MOCK_GPU_H
/* CPU-only ABI test doubles. These files never enter the product include path. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { kDLCPU=1, kDLCUDA=2 } DLDeviceType;
enum { kDLUInt=1, kDLFloat=2 };
typedef struct { DLDeviceType device_type; int32_t device_id; } DLDevice;
typedef struct { uint8_t code, bits; uint16_t lanes; } DLDataType;
typedef struct { void *data; DLDevice device; int32_t ndim; DLDataType dtype;
  int64_t *shape, *strides; uint64_t byte_offset; } DLTensor;
typedef struct DLManagedTensor { DLTensor dl_tensor; void *manager_ctx;
  void (*deleter)(struct DLManagedTensor *); } DLManagedTensor;
typedef struct MockStream *cudaStream_t;
typedef int cudaError_t;
enum { cudaSuccess=0, cudaStreamNonBlocking=1 };
typedef enum { cudaMemcpyHostToDevice=1, cudaMemcpyDeviceToHost=2, cudaMemcpyDefault=4 } cudaMemcpyKind;
cudaError_t cudaGetDevice(int *);
cudaError_t cudaSetDevice(int);
cudaError_t cudaStreamCreateWithFlags(cudaStream_t *, unsigned int);
cudaError_t cudaStreamDestroy(cudaStream_t);
cudaError_t cudaMemcpyAsync(void *, const void *, size_t, cudaMemcpyKind, cudaStream_t);
cudaError_t cudaMemcpy(void *, const void *, size_t, cudaMemcpyKind);
typedef uintptr_t cuvsResources_t;
typedef enum { CUVS_ERROR=0, CUVS_SUCCESS=1 } cuvsError_t;
typedef struct MockIndex *cuvsCagraIndex_t;
typedef struct MockParams *cuvsCagraIndexParams_t;
typedef struct MockParams *cuvsCagraSearchParams_t;
enum { NO_FILTER=0 };
typedef struct { int type; uintptr_t addr; } cuvsFilter;
const char *cuvsGetLastErrorText(void);
cuvsError_t cuvsResourcesCreate(cuvsResources_t *);
cuvsError_t cuvsResourcesDestroy(cuvsResources_t);
cuvsError_t cuvsStreamSet(cuvsResources_t, cudaStream_t);
cuvsError_t cuvsStreamSync(cuvsResources_t);
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void **, size_t);
cuvsError_t cuvsRMMFree(cuvsResources_t, void *, size_t);
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *);
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t);
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *);
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t);
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *);
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t);
cuvsError_t cuvsCagraBuild(cuvsResources_t, cuvsCagraIndexParams_t, DLManagedTensor *, cuvsCagraIndex_t);
cuvsError_t cuvsCagraSearch(cuvsResources_t, cuvsCagraSearchParams_t, cuvsCagraIndex_t,
  DLManagedTensor *, DLManagedTensor *, DLManagedTensor *, cuvsFilter);
enum MockFailure { MOCK_NONE=0, MOCK_RESOURCE, MOCK_STREAM_CREATE, MOCK_STREAM_SET,
  MOCK_INDEX_PARAMS, MOCK_SEARCH_PARAMS, MOCK_INDEX, MOCK_BUILD, MOCK_ALLOC,
  MOCK_COPY, MOCK_SEARCH, MOCK_SYNC, MOCK_DEVICE };
typedef struct { size_t allocations, frees, live_bytes, search_params, resources,
  indexes, params_live, streams, copies, searches, syncs; } MockStats;
MockStats mock_stats(void);
void mock_fail_next(enum MockFailure);
void mock_fail_copy(int);
int mock_pending(void);
#ifdef __cplusplus
}
#endif
#endif
