// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include "lib/allocator/page_arena.h"
#include "storage/retrieval/ob_sparse_daat_iter.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); \
} } while (false)

namespace
{
void compare_ids(ObObjType type, ObCollationType collation, bool bench)
{
  ObArenaAllocator allocator("FTSIdCmpTest");
  ObFixedArray<const ObDatum *, ObIAllocator> ids(&allocator);
  CHECK(ids.init(2) == OB_SUCCESS);
  CHECK(ids.prepare_allocate(2) == OB_SUCCESS);
  ObDatum left, right;
  uint64_t left_value = 0, right_value = 0;
  ids.get_data()[0] = &left;
  ids.get_data()[1] = &right;
  ObSRMergeCmp comparator;
  ObSRMergeItem l, r;
  l.iter_idx_ = 0;
  r.iter_idx_ = 1;
  CHECK(comparator.init(oceanbase::sql::ObDatumMeta(type, collation, 0), &ids) == OB_SUCCESS);
  const auto *funcs = ObDatumFuncs::get_basic_func(type, collation);
  CHECK(funcs != nullptr && funcs->null_first_cmp_ != nullptr);
  auto check = [&] {
    int expected = 0;
    int64_t actual = 0;
    CHECK(funcs->null_first_cmp_(left, right, expected, nullptr) == OB_SUCCESS);
    CHECK(comparator.cmp(l, r, actual) == OB_SUCCESS);
    CHECK((expected < 0) == (actual < 0));
    CHECK((expected > 0) == (actual > 0));
  };
  std::mt19937_64 random(20261029);
  std::string lbuf, rbuf;
  for (int i = 0; i < 100000; ++i) {
    if (type == ObUInt64Type) {
      left.ptr_ = reinterpret_cast<const char *>(&left_value);
      right.ptr_ = reinterpret_cast<const char *>(&right_value);
      left.set_uint(i % 7 == 0 ? UINT64_MAX : random());
      right.set_uint(i % 5 == 0 ? 0 : random());
    } else {
      lbuf.assign(random() % 41, ' ');
      rbuf.assign(random() % 41, ' ');
      for (char &c : lbuf) { c = static_cast<char>(random()); }
      for (char &c : rbuf) { c = static_cast<char>(random()); }
      // Valid ASCII also exercises PAD SPACE fallback collations.
      if (collation != CS_TYPE_BINARY) {
        lbuf = i % 2 ? "doc " : "doc";
        rbuf = i % 3 ? "doc" : "DOC";
      }
      left.set_string(lbuf.data(), lbuf.size());
      right.set_string(rbuf.data(), rbuf.size());
    }
    if (i % 11 == 0) { right = left; }
    check();
    if (i % 17 == 0) { left.set_null(); }
    if (i % 19 == 0) { right.set_null(); }
    check();
  }
  if (type == ObVarcharType) {
    const std::string boundaries[] = {
      "", "doc", "doc ", "DOC", std::string("doc\0", 4), std::string("doc\0 ", 5)};
    for (const auto &lv : boundaries) {
      for (const auto &rv : boundaries) {
        left.set_string(lv.data(), lv.size());
        right.set_string(rv.data(), rv.size());
        check();
      }
    }
  }
  if (bench) {
    if (type == ObUInt64Type) {
      left.ptr_ = reinterpret_cast<const char *>(&left_value);
      right.ptr_ = reinterpret_cast<const char *>(&right_value);
      left.set_uint(12345);
      right.set_uint(54321);
    } else {
      lbuf = "prefix-document-a";
      rbuf = "prefix-document-b";
      left.set_string(lbuf.data(), lbuf.size());
      right.set_string(rbuf.data(), rbuf.size());
    }
    constexpr int rounds = 3000000;
    for (bool optimized : {false, true}) {
      int64_t checksum = 0;
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < rounds; ++i) {
        if (optimized) {
          int64_t cmp = 0;
          CHECK(comparator.cmp(l, r, cmp) == OB_SUCCESS);
          checksum += cmp < 0;
        } else {
          int cmp = 0;
          CHECK(funcs->null_first_cmp_(left, right, cmp, nullptr) == OB_SUCCESS);
          checksum += cmp < 0;
        }
      }
      const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
      std::printf("docid_%d_%d_%s_ns=%.3f checksum=%lld\n", type, collation,
          optimized ? "specialized" : "datum", ns / rounds, static_cast<long long>(checksum));
    }
  }
}
}

int main(int argc, char **argv)
{
  CHECK(ObCharset::init_charset() == OB_SUCCESS);
  const bool bench = argc > 1 && std::string(argv[1]) == "--bench";
  compare_ids(ObUInt64Type, CS_TYPE_BINARY, bench);
  compare_ids(ObVarcharType, CS_TYPE_BINARY, bench);
  compare_ids(ObVarcharType, CS_TYPE_UTF8MB4_BIN, false);
  compare_ids(ObVarcharType, CS_TYPE_UTF8MB4_GENERAL_CI, false);
  std::puts("FTS document ID comparator differential regressions passed");
}
