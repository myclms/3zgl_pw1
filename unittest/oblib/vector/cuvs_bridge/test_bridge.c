#include "mock_gpu.h"
#include "seekdb_cuvs_bridge.h"
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);return 1;} } while(0)
static float base[]={0,0,10,0,20,0,30,0};
static int clean(void){MockStats s=mock_stats();return s.live_bytes==0&&s.resources==0&&s.indexes==0&&s.params_live==0&&s.streams==0;}
static int query_ok(void *h){unsigned ids[2]={99,99};float distances[2]={-1,-1},q[]={9,0};
  CHECK(seekdb_cuvs_search(h,q,1,2,ids,distances)==0);CHECK(ids[0]==1&&ids[1]==0);CHECK(fabsf(distances[0]-1)<1e-6f&&fabsf(distances[1]-81)<1e-6f);return 0;}
static int reuse_case(void){
  void *h=seekdb_cuvs_build(base,4,2);CHECK(h);CHECK(query_ok(h)==0);
  MockStats first=mock_stats();for(int i=0;i<20;i++)CHECK(query_ok(h)==0);
  MockStats after=mock_stats();CHECK(after.allocations==first.allocations);CHECK(after.search_params==first.search_params);
  seekdb_cuvs_free(h);CHECK(clean());return 0;
}
static int ready_and_shapes(void){
  float source[]={0,0,10,0,20,0,30,0};void *h=seekdb_cuvs_build(source,4,2);CHECK(h);
  memset(source,0,sizeof(source));CHECK(query_ok(h)==0);
  float queries[]={0,0,10,0,20,0};unsigned ids[9];float distances[9];
  CHECK(seekdb_cuvs_search(h,queries,3,3,ids,distances)==0);CHECK(ids[0]==0&&ids[3]==1&&ids[6]==2);
  CHECK(seekdb_cuvs_search(h,queries,1,1,ids,NULL)==0&&ids[0]==0);
  CHECK(cudaSetDevice(1)==cudaSuccess);CHECK(query_ok(h)==0);int device=0;cudaGetDevice(&device);CHECK(device==1);
  seekdb_cuvs_free(h);cudaGetDevice(&device);CHECK(device==1);cudaSetDevice(0);CHECK(clean());return 0;
}
static int invalid_arguments(void){
  CHECK(seekdb_cuvs_build(NULL,4,2)==NULL);CHECK(seekdb_cuvs_build(base,0,2)==NULL);CHECK(seekdb_cuvs_build(base,4,0)==NULL);
  void *h=seekdb_cuvs_build(base,4,2);CHECK(h);unsigned ids[2];float q[]={9,0};
  CHECK(seekdb_cuvs_search(NULL,q,1,1,ids,NULL)!=0);CHECK(seekdb_cuvs_search(h,NULL,1,1,ids,NULL)!=0);
  CHECK(seekdb_cuvs_search(h,q,0,1,ids,NULL)!=0);CHECK(seekdb_cuvs_search(h,q,1,0,ids,NULL)!=0);
  CHECK(seekdb_cuvs_search(h,q,1,5,ids,NULL)!=0);CHECK(seekdb_cuvs_search(h,q,1,1,NULL,NULL)!=0);
  CHECK(seekdb_cuvs_search(h,q,LONG_MAX,LONG_MAX,ids,NULL)!=0);
  seekdb_cuvs_free(h);seekdb_cuvs_free(NULL);CHECK(clean());return 0;
}
static int initialization_failures(void){
  enum MockFailure failures[]={MOCK_DEVICE,MOCK_RESOURCE,MOCK_STREAM_CREATE,MOCK_STREAM_SET,MOCK_INDEX_PARAMS,MOCK_SEARCH_PARAMS,MOCK_INDEX,MOCK_BUILD,MOCK_SYNC};
  for(size_t i=0;i<sizeof(failures)/sizeof(failures[0]);i++){mock_fail_next(failures[i]);CHECK(seekdb_cuvs_build(base,4,2)==NULL);CHECK(clean());}
  return 0;
}
static int search_failures(void){
  void *h=seekdb_cuvs_build(base,4,2);CHECK(h);float q[]={9,0};unsigned ids[2];float distances[2];
  mock_fail_next(MOCK_ALLOC);CHECK(seekdb_cuvs_search(h,q,1,2,ids,distances)!=0);CHECK(query_ok(h)==0);
  for(int i=1;i<=3;i++){mock_fail_copy(i);CHECK(seekdb_cuvs_search(h,q,1,2,ids,distances)!=0);CHECK(query_ok(h)==0);}
  mock_fail_next(MOCK_SEARCH);CHECK(seekdb_cuvs_search(h,q,1,2,ids,distances)!=0);CHECK(query_ok(h)==0);
  mock_fail_next(MOCK_SYNC);CHECK(seekdb_cuvs_search(h,q,1,2,ids,distances)!=0);CHECK(query_ok(h)==0);
  mock_fail_next(MOCK_DEVICE);CHECK(seekdb_cuvs_search(h,q,1,2,ids,distances)!=0);CHECK(query_ok(h)==0);
  float large[200]={0};unsigned large_ids[100];mock_fail_next(MOCK_ALLOC);
  CHECK(seekdb_cuvs_search(h,large,100,1,large_ids,NULL)!=0);CHECK(query_ok(h)==0);
  seekdb_cuvs_free(h);CHECK(clean());return 0;
}
static int independent_handles(void){
  void *a=seekdb_cuvs_build(base,4,2);float other[]={100,0,200,0};void *b=seekdb_cuvs_build(other,2,2);CHECK(a&&b);
  CHECK(query_ok(a)==0);float q[]={190,0};unsigned id=0;CHECK(seekdb_cuvs_search(b,q,1,1,&id,NULL)==0&&id==1);
  seekdb_cuvs_free(a);CHECK(seekdb_cuvs_search(b,q,1,1,&id,NULL)==0&&id==1);seekdb_cuvs_free(b);CHECK(clean());return 0;
}
typedef struct { void *handle; int id, failed; } ThreadCase;
static void *thread_search(void *data){ThreadCase *t=(ThreadCase*)data;
  for(int i=0;i<30;i++){float q[]={(float)(t->id*10),0};unsigned id=99;float distance=-1;
    if(seekdb_cuvs_search(t->handle,q,1,1,&id,&distance)!=0||id!=(unsigned)t->id||distance!=0)t->failed=1;}
  return NULL;
}
static int concurrent_queries(void){
  void *h=seekdb_cuvs_build(base,4,2);CHECK(h);pthread_t threads[4];ThreadCase cases[4];
  for(int i=0;i<4;i++){cases[i].handle=h;cases[i].id=i;cases[i].failed=0;CHECK(pthread_create(&threads[i],NULL,thread_search,&cases[i])==0);}
  for(int i=0;i<4;i++){pthread_join(threads[i],NULL);CHECK(!cases[i].failed);}
  seekdb_cuvs_free(h);CHECK(clean());return 0;
}
static int retention_bound(void){
  const long count=(64L*1024L*1024L)/16+1;void *h=seekdb_cuvs_build(base,4,2);CHECK(h);
  float *q=(float*)calloc((size_t)count*2,sizeof(float));unsigned *ids=(unsigned*)malloc((size_t)count*sizeof(unsigned));float *dist=(float*)malloc((size_t)count*sizeof(float));CHECK(q&&ids&&dist);
  CHECK(seekdb_cuvs_search(h,q,count,1,ids,dist)==0);CHECK(ids[0]==0&&ids[count-1]==0);CHECK(mock_stats().live_bytes==0);
  free(q);free(ids);free(dist);CHECK(query_ok(h)==0);seekdb_cuvs_free(h);CHECK(clean());return 0;
}
static int v5_large_topk(void){
  float vectors[400];for(int i=0;i<200;i++){vectors[i*2]=(float)i;vectors[i*2+1]=0;}
  void *h=seekdb_cuvs_build(vectors,200,2);CHECK(h);float q[]={0,0};unsigned ids[150];float distances[150];
  CHECK(seekdb_cuvs_search(h,q,1,150,ids,distances)==0);CHECK(ids[149]==149);
  CHECK(mock_stats().last_itopk_size==600&&mock_stats().last_search_width==4);
  CHECK(seekdb_cuvs_search(h,q,1,100,ids,distances)==0);CHECK(ids[99]==99);
  CHECK(mock_stats().last_itopk_size==512);
  CHECK(seekdb_cuvs_search(h,q,1,10,ids,distances)==0);CHECK(ids[9]==9&&mock_stats().last_itopk_size==512);
  seekdb_cuvs_free(h);CHECK(clean());return 0;
}
static int snapshot_lifecycle(void){
  const char *path="index.mock";void *h=seekdb_cuvs_build(base,4,2);CHECK(h);
  CHECK(seekdb_cuvs_serialize(NULL,path)!=0);CHECK(seekdb_cuvs_serialize(h,NULL)!=0);CHECK(seekdb_cuvs_serialize(h,"")!=0);
  CHECK(cudaSetDevice(1)==cudaSuccess);CHECK(seekdb_cuvs_serialize(h,path)==0);
  int device=0;cudaGetDevice(&device);CHECK(device==1);CHECK(query_ok(h)==0);
  mock_fail_next(MOCK_SERIALIZE);CHECK(seekdb_cuvs_serialize(h,path)!=0);CHECK(query_ok(h)==0);
  mock_fail_next(MOCK_SYNC);CHECK(seekdb_cuvs_serialize(h,path)!=0);CHECK(query_ok(h)==0);
  mock_fail_next(MOCK_DEVICE);CHECK(seekdb_cuvs_serialize(h,path)!=0);CHECK(query_ok(h)==0);
  CHECK(seekdb_cuvs_serialize(h,"missing-directory/index.mock")!=0);
  seekdb_cuvs_free(h);CHECK(clean());cudaSetDevice(0);
  CHECK(seekdb_cuvs_deserialize(NULL,4,2)==NULL);CHECK(seekdb_cuvs_deserialize("",4,2)==NULL);
  CHECK(seekdb_cuvs_deserialize(path,0,2)==NULL);CHECK(seekdb_cuvs_deserialize(path,4,0)==NULL);
  CHECK(seekdb_cuvs_deserialize("missing.mock",4,2)==NULL);CHECK(clean());
  CHECK(seekdb_cuvs_deserialize(path,3,2)==NULL);CHECK(seekdb_cuvs_deserialize(path,4,3)==NULL);CHECK(clean());
  enum MockFailure failures[]={MOCK_RESOURCE,MOCK_STREAM_CREATE,MOCK_STREAM_SET,MOCK_INDEX_PARAMS,MOCK_SEARCH_PARAMS,MOCK_INDEX,MOCK_DESERIALIZE,MOCK_SYNC,MOCK_GET_SIZE,MOCK_GET_DIMS};
  for(size_t i=0;i<sizeof(failures)/sizeof(failures[0]);i++){mock_fail_next(failures[i]);CHECK(seekdb_cuvs_deserialize(path,4,2)==NULL);CHECK(clean());}
  FILE *bad=fopen("truncated.mock","wb");CHECK(bad);CHECK(fputc(1,bad)!=EOF);CHECK(fclose(bad)==0);
  CHECK(seekdb_cuvs_deserialize("truncated.mock",4,2)==NULL);CHECK(clean());
  h=seekdb_cuvs_deserialize(path,4,2);CHECK(h);CHECK(query_ok(h)==0);
  MockStats first=mock_stats();CHECK(query_ok(h)==0);CHECK(first.allocations==mock_stats().allocations);
  seekdb_cuvs_free(h);CHECK(clean());CHECK(remove(path)==0);CHECK(remove("truncated.mock")==0);return 0;
}
int main(int argc,char **argv){
  if(argc>1&&strcmp(argv[1],"reuse-only")==0)return reuse_case();
  struct { const char *name; int (*run)(void); } tests[]={
    {"reuse_same_shape",reuse_case},{"async_completion_and_shapes",ready_and_shapes},
    {"invalid_arguments",invalid_arguments},{"partial_initialization_cleanup",initialization_failures},
    {"copy_search_sync_allocation_failures",search_failures},{"index_isolation",independent_handles},
    {"concurrent_same_handle",concurrent_queries},{"bounded_workspace_retention",retention_bound},
    {"v5_large_topk_quality_settings",v5_large_topk},{"snapshot_roundtrip_and_failure_cleanup",snapshot_lifecycle}};
  for(size_t i=0;i<sizeof(tests)/sizeof(tests[0]);i++){if(tests[i].run())return 1;printf("PASS %s\n",tests[i].name);}
  printf("MOCK_ONLY: real_gpu=false; cases=10; GPU quality/performance NOT measured\n");return 0;
}
