#include "mock_gpu.h"
#include <assert.h>
#include <float.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct MockIndex { float *base; long n, dim; };
struct MockParams { int unused; };
typedef struct Op {
  int kind; struct Op *next; void *dst; const void *src; size_t bytes;
  struct MockIndex *index; long nq, dim, k; unsigned *ids; float *dist;
} Op;
struct MockStream { Op *head, *tail; pthread_mutex_t mutex; };
typedef struct { cudaStream_t stream; } Resource;
typedef struct Memory { void *ptr; size_t bytes; struct Memory *next; } Memory;
static Memory *memory;
static struct MockStream default_stream={NULL,NULL,PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t state_mutex=PTHREAD_MUTEX_INITIALIZER;
static MockStats stats;
static enum MockFailure failure;
static int fail_copy_count;
static _Thread_local int current_device;
static int failed(enum MockFailure kind) {
  int result=0; pthread_mutex_lock(&state_mutex);
  if(failure==kind){failure=MOCK_NONE;result=1;}
  pthread_mutex_unlock(&state_mutex);return result;
}
void mock_fail_next(enum MockFailure kind){failure=kind;}
void mock_fail_copy(int n){fail_copy_count=n;}
MockStats mock_stats(void){return stats;}
const char *cuvsGetLastErrorText(void){return "injected CPU test-double failure";}
static void enqueue(cudaStream_t stream,Op *op){
  if(!stream)stream=&default_stream;
  pthread_mutex_lock(&stream->mutex);if(stream->tail)stream->tail->next=op;else stream->head=op;
  stream->tail=op;pthread_mutex_unlock(&stream->mutex);
}
static void flush(cudaStream_t stream){
  if(!stream)stream=&default_stream;
  pthread_mutex_lock(&stream->mutex);
  Op *op=stream->head;stream->head=stream->tail=NULL;
  while(op){Op *next=op->next;
    if(op->kind==1)memcpy(op->dst,op->src,op->bytes);
    else if(op->kind==2){op->index->base=(float*)malloc(op->bytes);assert(op->index->base);memcpy(op->index->base,op->src,op->bytes);}
    else {
      const float *q=(const float*)op->src;struct MockIndex *idx=op->index;assert(idx->base);
      for(long row=0;row<op->nq;row++){
        for(long k=0;k<op->k;k++){op->ids[row*op->k+k]=UINT32_MAX;op->dist[row*op->k+k]=FLT_MAX;}
        for(long id=0;id<idx->n;id++){
          float distance=0;for(long d=0;d<idx->dim;d++){float diff=q[row*idx->dim+d]-idx->base[id*idx->dim+d];distance+=diff*diff;}
          for(long k=0;k<op->k;k++)if(distance<op->dist[row*op->k+k]){
            for(long j=op->k-1;j>k;j--){op->dist[row*op->k+j]=op->dist[row*op->k+j-1];op->ids[row*op->k+j]=op->ids[row*op->k+j-1];}
            op->dist[row*op->k+k]=distance;op->ids[row*op->k+k]=(unsigned)id;break;
          }
        }
      }
    }
    free(op);op=next;
  }
  pthread_mutex_unlock(&stream->mutex);
}
int mock_pending(void){return default_stream.head!=NULL;}
cudaError_t cudaGetDevice(int *device){if(failed(MOCK_DEVICE))return 1;*device=current_device;return cudaSuccess;}
cudaError_t cudaSetDevice(int device){current_device=device;return cudaSuccess;}
cudaError_t cudaStreamCreateWithFlags(cudaStream_t *stream,unsigned int flags){
  (void)flags;if(failed(MOCK_STREAM_CREATE))return 1;
  *stream=(cudaStream_t)calloc(1,sizeof(**stream));assert(*stream);pthread_mutex_init(&(*stream)->mutex,NULL);stats.streams++;return cudaSuccess;
}
cudaError_t cudaStreamDestroy(cudaStream_t stream){flush(stream);pthread_mutex_destroy(&stream->mutex);free(stream);stats.streams--;return cudaSuccess;}
cudaError_t cudaMemcpyAsync(void *dst,const void *src,size_t bytes,cudaMemcpyKind kind,cudaStream_t stream){
  (void)kind;if(failed(MOCK_COPY)||(fail_copy_count>0&&--fail_copy_count==0))return 1;
  Op *op=(Op*)calloc(1,sizeof(*op));assert(op);op->kind=1;op->dst=dst;op->src=src;op->bytes=bytes;enqueue(stream,op);stats.copies++;return cudaSuccess;
}
cudaError_t cudaMemcpy(void *dst,const void *src,size_t bytes,cudaMemcpyKind kind){
  (void)kind;flush(&default_stream);memcpy(dst,src,bytes);return cudaSuccess;
}
cuvsError_t cuvsResourcesCreate(cuvsResources_t *res){
  if(failed(MOCK_RESOURCE))return CUVS_ERROR;
  Resource *r=(Resource*)calloc(1,sizeof(*r));assert(r);r->stream=&default_stream;*res=(uintptr_t)r;stats.resources++;return CUVS_SUCCESS;
}
cuvsError_t cuvsResourcesDestroy(cuvsResources_t res){Resource *r=(Resource*)res;flush(r->stream);free(r);stats.resources--;return CUVS_SUCCESS;}
cuvsError_t cuvsStreamSet(cuvsResources_t res,cudaStream_t stream){if(failed(MOCK_STREAM_SET))return CUVS_ERROR;((Resource*)res)->stream=stream;return CUVS_SUCCESS;}
cuvsError_t cuvsStreamSync(cuvsResources_t res){flush(((Resource*)res)->stream);stats.syncs++;return failed(MOCK_SYNC)?CUVS_ERROR:CUVS_SUCCESS;}
cuvsError_t cuvsRMMAlloc(cuvsResources_t res,void **ptr,size_t bytes){
  (void)res;if(failed(MOCK_ALLOC)||bytes>256UL*1024*1024)return CUVS_ERROR;
  Memory *m=(Memory*)calloc(1,sizeof(*m));assert(m);m->ptr=malloc(bytes);assert(m->ptr);m->bytes=bytes;
  m->next=memory;memory=m;*ptr=m->ptr;stats.allocations++;stats.live_bytes+=bytes;return CUVS_SUCCESS;
}
cuvsError_t cuvsRMMFree(cuvsResources_t res,void *ptr,size_t bytes){
  flush(((Resource*)res)->stream);Memory **p=&memory;while(*p&&(*p)->ptr!=ptr)p=&(*p)->next;
  assert(*p&&(*p)->bytes==bytes);Memory *m=*p;*p=m->next;free(m->ptr);free(m);stats.frees++;stats.live_bytes-=bytes;return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *p){if(failed(MOCK_INDEX_PARAMS))return CUVS_ERROR;*p=(struct MockParams*)calloc(1,sizeof(**p));assert(*p);stats.params_live++;return CUVS_SUCCESS;}
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t p){free(p);stats.params_live--;return CUVS_SUCCESS;}
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *p){if(failed(MOCK_SEARCH_PARAMS))return CUVS_ERROR;*p=(struct MockParams*)calloc(1,sizeof(**p));assert(*p);stats.params_live++;stats.search_params++;return CUVS_SUCCESS;}
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p){free(p);stats.params_live--;return CUVS_SUCCESS;}
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *p){if(failed(MOCK_INDEX))return CUVS_ERROR;*p=(struct MockIndex*)calloc(1,sizeof(**p));assert(*p);stats.indexes++;return CUVS_SUCCESS;}
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t p){free(p->base);free(p);stats.indexes--;return CUVS_SUCCESS;}
cuvsError_t cuvsCagraBuild(cuvsResources_t res,cuvsCagraIndexParams_t p,DLManagedTensor *data,cuvsCagraIndex_t idx){
  (void)p;if(failed(MOCK_BUILD)||!data->dl_tensor.data||data->dl_tensor.shape[0]<=0||data->dl_tensor.shape[1]<=0)return CUVS_ERROR;
  idx->n=(long)data->dl_tensor.shape[0];idx->dim=(long)data->dl_tensor.shape[1];
  Op *op=(Op*)calloc(1,sizeof(*op));assert(op);op->kind=2;op->src=data->dl_tensor.data;op->bytes=(size_t)idx->n*idx->dim*sizeof(float);op->index=idx;enqueue(((Resource*)res)->stream,op);return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearch(cuvsResources_t res,cuvsCagraSearchParams_t p,cuvsCagraIndex_t idx,DLManagedTensor *q,DLManagedTensor *ids,DLManagedTensor *dist,cuvsFilter filter){
  (void)p;(void)filter;if(failed(MOCK_SEARCH))return CUVS_ERROR;
  assert(q->dl_tensor.dtype.bits==32&&ids->dl_tensor.dtype.code==kDLUInt);
  Op *op=(Op*)calloc(1,sizeof(*op));assert(op);op->kind=3;op->index=idx;op->src=q->dl_tensor.data;op->nq=(long)q->dl_tensor.shape[0];op->k=(long)ids->dl_tensor.shape[1];op->ids=(unsigned*)ids->dl_tensor.data;op->dist=(float*)dist->dl_tensor.data;stats.searches++;enqueue(((Resource*)res)->stream,op);return CUVS_SUCCESS;
}
