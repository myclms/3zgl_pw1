/* seekdb <-> hipVS bridge. Public data and CAGRA parameters remain FP32. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Retain workspace, not graphs from another index or test round. Large batches
 * run normally but release their workspace on return. */
#define SK_WORKSPACE_CACHE_LIMIT (64UL * 1024UL * 1024UL)

typedef struct { void *data; size_t bytes; } sk_device_buffer;
typedef struct {
  cuvsResources_t res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraSearchParams_t sp;
  cuvsCagraIndex_t idx;
  cudaStream_t stream;
  int stream_created, device;
  pthread_mutex_t mutex;
  sk_device_buffer query, neighbors, distances;
  long n, dim;
} sk_cuvs_index;

static int sk_bytes(long rows, long cols, size_t item_size, size_t *out)
{
  if (rows <= 0 || cols <= 0 || (uint64_t)rows > SIZE_MAX / item_size ||
      (uint64_t)cols > SIZE_MAX / ((size_t)rows * item_size)) return 1;
  *out = (size_t)rows * (size_t)cols * item_size;
  return 0;
}

static DLManagedTensor sk_tensor(void *data, DLDeviceType device_type, int device,
                                 uint8_t code, int64_t shape[2])
{
  DLManagedTensor tensor;
  memset(&tensor, 0, sizeof(tensor));
  tensor.dl_tensor.data = data;
  tensor.dl_tensor.device.device_type = device_type;
  tensor.dl_tensor.device.device_id = device;
  tensor.dl_tensor.ndim = 2;
  tensor.dl_tensor.dtype.code = code;
  tensor.dl_tensor.dtype.bits = 32;
  tensor.dl_tensor.dtype.lanes = 1;
  tensor.dl_tensor.shape = shape;
  return tensor;
}

static int sk_release_buffer(sk_cuvs_index *h, sk_device_buffer *buffer)
{
  if (buffer->data == NULL) return 0;
  if (cuvsRMMFree(h->res, buffer->data, buffer->bytes) != CUVS_SUCCESS) return 1;
  buffer->data = NULL;
  buffer->bytes = 0;
  return 0;
}

static int sk_reserve(sk_cuvs_index *h, sk_device_buffer *buffer, size_t bytes)
{
  if (buffer->data != NULL && buffer->bytes >= bytes) return 0;
  /* Free before growing, avoiding a temporary doubling of peak workspace. An
   * allocation failure leaves an empty slot that a later call can retry. */
  if (sk_release_buffer(h, buffer)) return 1;
  void *data = NULL;
  if (cuvsRMMAlloc(h->res, &data, bytes) != CUVS_SUCCESS) return 1;
  buffer->data = data;
  buffer->bytes = bytes;
  return 0;
}

static void sk_release_workspace(sk_cuvs_index *h)
{
  sk_release_buffer(h, &h->query);
  sk_release_buffer(h, &h->neighbors);
  sk_release_buffer(h, &h->distances);
}

static void sk_destroy(sk_cuvs_index *h)
{
  if (h->res) {
    cuvsStreamSync(h->res);
    sk_release_workspace(h);
    if (h->idx) cuvsCagraIndexDestroy(h->idx);
    if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
    if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
    cuvsStreamSync(h->res);
    cuvsResourcesDestroy(h->res);
  }
  if (h->stream_created) cudaStreamDestroy(h->stream);
}

void *seekdb_cuvs_build(const float *base, long n, long dim)
{
  size_t dataset_bytes;
  int64_t shape[2] = {n, dim};
  DLManagedTensor dataset = sk_tensor((void *)base, kDLCPU, 0, kDLFloat, shape);
  if (base == NULL || (uint64_t)n > UINT32_MAX ||
      sk_bytes(n, dim, sizeof(float), &dataset_bytes)) return NULL;
  sk_cuvs_index *h = (sk_cuvs_index *)calloc(1, sizeof(*h));
  if (h == NULL) return NULL;
  if (pthread_mutex_init(&h->mutex, NULL) != 0) { free(h); return NULL; }
  if (cudaGetDevice(&h->device) != cudaSuccess ||
      cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) goto fail;
  if (cudaStreamCreateWithFlags(&h->stream, cudaStreamNonBlocking) != cudaSuccess) goto fail;
  h->stream_created = 1;
  if (cuvsStreamSet(h->res, h->stream) != CUVS_SUCCESS ||
      cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS ||
      cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS ||
      cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail;
  /* Preserve the frozen baseline's metric, degrees, build algorithm and search
   * parameters. Do not trade recall for speed. */
  if (cuvsCagraBuild(h->res, h->ip, &dataset, h->idx) != CUVS_SUCCESS ||
      cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail;
  h->n = n;
  h->dim = dim;
  return h;
fail:
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  sk_destroy(h);
  pthread_mutex_destroy(&h->mutex);
  free(h);
  return NULL;
}

int seekdb_cuvs_search(void *handle, const float *query, long nq, long topk,
                       unsigned int *out_ids, float *out_dist)
{
  sk_cuvs_index *h = (sk_cuvs_index *)handle;
  size_t query_bytes, id_bytes, dist_bytes;
  if (h == NULL || query == NULL || out_ids == NULL || topk > h->n ||
      sk_bytes(nq, h->dim, sizeof(float), &query_bytes) ||
      sk_bytes(nq, topk, sizeof(unsigned int), &id_bytes) ||
      sk_bytes(nq, topk, sizeof(float), &dist_bytes)) return 1;
  if (pthread_mutex_lock(&h->mutex) != 0) return 2;
  int rc = 0, previous_device = -1, device_ready = 0;
  if (cudaGetDevice(&previous_device) != cudaSuccess ||
      (previous_device != h->device && cudaSetDevice(h->device) != cudaSuccess)) {
    rc = 3; goto done;
  }
  device_ready = 1;
  if (sk_reserve(h, &h->query, query_bytes)) { rc = 10; goto done; }
  if (sk_reserve(h, &h->neighbors, id_bytes)) { rc = 11; goto done; }
  if (sk_reserve(h, &h->distances, dist_bytes)) { rc = 12; goto done; }
  if (cudaMemcpyAsync(h->query.data, query, query_bytes, cudaMemcpyHostToDevice,
                      h->stream) != cudaSuccess) { rc = 13; goto done; }
  {
    int64_t query_shape[2] = {nq, h->dim}, output_shape[2] = {nq, topk};
    DLManagedTensor qt = sk_tensor(h->query.data, kDLCUDA, h->device, kDLFloat, query_shape);
    DLManagedTensor nt = sk_tensor(h->neighbors.data, kDLCUDA, h->device, kDLUInt, output_shape);
    DLManagedTensor dt = sk_tensor(h->distances.data, kDLCUDA, h->device, kDLFloat, output_shape);
    cuvsFilter filter;
    filter.type = NO_FILTER;
    filter.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, filter) != CUVS_SUCCESS) {
      rc = 15; goto done;
    }
  }
  if (cudaMemcpyAsync(out_ids, h->neighbors.data, id_bytes, cudaMemcpyDeviceToHost,
                      h->stream) != cudaSuccess) { rc = 16; goto done; }
  if (out_dist != NULL &&
      cudaMemcpyAsync(out_dist, h->distances.data, dist_bytes, cudaMemcpyDeviceToHost,
                      h->stream) != cudaSuccess) { rc = 17; goto done; }
 done:
  /* Include the fence in the public API's latency, also on error paths. */
  if (device_ready && cuvsStreamSync(h->res) != CUVS_SUCCESS && rc == 0) rc = 18;
  if (device_ready && (h->query.bytes > SK_WORKSPACE_CACHE_LIMIT ||
      h->neighbors.bytes > SK_WORKSPACE_CACHE_LIMIT - h->query.bytes ||
      h->distances.bytes > SK_WORKSPACE_CACHE_LIMIT - h->query.bytes - h->neighbors.bytes)) {
    sk_release_workspace(h);
  }
  if (device_ready && previous_device != h->device &&
      cudaSetDevice(previous_device) != cudaSuccess && rc == 0) rc = 19;
  pthread_mutex_unlock(&h->mutex);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d\n", rc);
  return rc;
}

void seekdb_cuvs_free(void *handle)
{
  sk_cuvs_index *h = (sk_cuvs_index *)handle;
  if (h == NULL) return;
  /* As in the original ABI, the owner must stop submissions before free. */
  pthread_mutex_lock(&h->mutex);
  int previous_device = -1;
  if (cudaGetDevice(&previous_device) != cudaSuccess ||
      (previous_device != h->device && cudaSetDevice(h->device) != cudaSuccess)) {
    pthread_mutex_unlock(&h->mutex);
    fprintf(stderr, "[seekdb_cuvs] cannot activate index device for release\n");
    return;
  }
  sk_destroy(h);
  if (previous_device != h->device) cudaSetDevice(previous_device);
  pthread_mutex_unlock(&h->mutex);
  pthread_mutex_destroy(&h->mutex);
  free(h);
}

int seekdb_cuvs_cagra_knn(const float *base, long n, long dim,
                          const float *query, long nq, long topk,
                          unsigned int *out_ids)
{
  void *handle = seekdb_cuvs_build(base, n, dim);
  if (handle == NULL) return 10;
  int rc = seekdb_cuvs_search(handle, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(handle);
  return rc;
}
