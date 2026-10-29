// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_set>
#include <vector>
#include "storage/fts/dict/ob_ft_range_dict.h"
#include "storage/fts/dict/ob_ft_dat_dict.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); \
} } while (false)

namespace
{
class TrackingAllocator : public ObIAllocator
{
public:
  void *alloc(int64_t size) override
  {
    if (allowance_ == 0) { return nullptr; }
    if (allowance_ > 0) { --allowance_; }
    void *ptr = std::malloc(size);
    CHECK(ptr != nullptr && live_.insert(ptr).second);
    ++allocations_;
    return ptr;
  }
  void *alloc(int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *ptr) override
  {
    if (ptr != nullptr) {
      CHECK(live_.erase(ptr) == 1);
      std::free(ptr);
    }
  }
  ~TrackingAllocator() override { CHECK(live_.empty()); }
  int allowance_ = -1;
  int allocations_ = 0;
  std::unordered_set<void *> live_;
};

void check_ranges(int range_count)
{
  TrackingAllocator cache_allocator;
  ObFTCacheRangeContainer cache(cache_allocator);
  std::vector<ObFTDAT> blocks(range_count);
  std::vector<std::unique_ptr<ObDictCacheValue>> values;
  for (auto &block : blocks) {
    CHECK(block.start_word_.set_word("a", 1) == OB_SUCCESS);
    CHECK(block.end_word_.set_word("z", 1) == OB_SUCCESS);
    values.emplace_back(new ObDictCacheValue(&block));
    ObFTCacheRangeHandle *handle = nullptr;
    CHECK(cache.fetch_info_for_dict(handle) == OB_SUCCESS);
    handle->value_ = values.back().get();
  }
  const ObFTDictDesc desc("allocator_test", ObFTDictType::DICT_IK_MAIN,
                        CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
  // Fail every allocation in turn, including the first expansion beyond the
  // inline ranges. Already-created wrappers and the failed push must be freed.
  for (int allowance = 0; allowance <= range_count + 5; ++allowance) {
    TrackingAllocator allocator;
    allocator.allowance_ = allowance;
    {
      ObFTRangeDict dict(allocator, &cache, desc);
      int ret = dict.init();
      CHECK(ret == OB_SUCCESS || ret == OB_ALLOCATE_MEMORY_FAILED);
      if (ret == OB_SUCCESS && range_count != 0) {
        CHECK(allocator.allocations_ >= range_count);
        bool matched = true;
        CHECK(dict.match(ObString::make_string("~"), matched) == OB_SUCCESS && !matched);
      }
    }
    CHECK(allocator.live_.empty());
    // Destroying the borrowed readers must not destroy the cache or its data.
    CHECK(cache.get_handles().size() == range_count);
    if (range_count != 0) { CHECK(blocks.front().start_word_.get_word() == "a"); }
  }
  cache.reset();
  CHECK(cache_allocator.live_.empty());
}
}

int main()
{
  CHECK(ObCharset::init_charset() == OB_SUCCESS);
  for (int count : {0, 1, 4, 5, 12, 80}) { check_ranges(count); }
  std::puts("FTS range allocator ownership and failure regressions passed");
}
