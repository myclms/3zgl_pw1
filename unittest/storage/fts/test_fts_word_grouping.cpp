// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "lib/allocator/page_arena.h"
#include "storage/fts/ob_fts_stop_word.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); \
} } while (false)

namespace
{
ObObjMeta word_meta(ObCollationType collation)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(collation);
  return meta;
}

struct ReferenceStopwords
{
  hash::ObHashSet<ObFTWord> words;
  ObObjMeta meta = word_meta(CS_TYPE_UTF8MB4_GENERAL_CI);

  ReferenceStopwords()
  {
    CHECK(words.create(37, "FTSRefStopwords", "FTSRefStopwords") == OB_SUCCESS);
    for (const auto &word : ob_stop_word_list) {
      CHECK(words.set_refactored(ObFTWord(STRLEN(word), word, meta)) == OB_SUCCESS);
    }
  }
  ~ReferenceStopwords() { words.destroy(); }

  int check(const ObFTWord &word, bool &is_stopword)
  {
    ObArenaAllocator allocator("FTSRefStopwords");
    ObString converted;
    int ret = ObCharset::charset_convert(allocator, word.get_word().get_string(),
        word.get_collation_type(), meta.get_collation_type(), converted);
    if (ret == OB_SUCCESS) {
      ret = words.exist_refactored(ObFTWord(converted.length(), converted.ptr(), meta));
      if (ret == OB_HASH_EXIST || ret == OB_HASH_NOT_EXIST) {
        is_stopword = ret == OB_HASH_EXIST;
        ret = OB_SUCCESS;
      } else if (ret == OB_SUCCESS) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    return ret;
  }
};

void compare_stopword(ObStopWordChecker &actual, ReferenceStopwords &reference,
                      const std::string &input, ObCollationType collation)
{
  ObFTWord word(input.size(), input.data(), word_meta(collation));
  bool expected_result = false, actual_result = false;
  const int expected_ret = reference.check(word, expected_result);
  const int actual_ret = actual.check_stopword(word, actual_result);
  CHECK(actual_ret == expected_ret);
  CHECK(actual_result == expected_result);
}

void check_stopwords(ObStopWordChecker &checker, ReferenceStopwords &reference)
{
  const ObCollationType collations[] = {
    CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN, CS_TYPE_LATIN1_SWEDISH_CI,
    CS_TYPE_GBK_CHINESE_CI, CS_TYPE_UTF16_GENERAL_CI, CS_TYPE_UTF16LE_GENERAL_CI, CS_TYPE_BINARY};
  for (auto collation : collations) {
    if (ObCharset::get_charset(collation) == nullptr) { continue; }
    ObArenaAllocator allocator("FTSWordTest");
    std::vector<std::string> words = {
      "the", "THE", "with", "WITH", "thé", "thé ", "database", "中文", "a", "an", "www", "notstopword"};
    for (const auto &word : ob_stop_word_list) { words.emplace_back(word); }
    for (const auto &word : words) {
      ObString converted;
      if (ObCharset::charset_convert(allocator, ObString(word.size(), word.data()),
          CS_TYPE_UTF8MB4_GENERAL_CI, collation, converted) != OB_SUCCESS) { continue; }
      compare_stopword(checker, reference, std::string(converted.ptr(), converted.length()), collation);
    }
    // Preserve conversion errors as well as successful membership checks.
    compare_stopword(checker, reference, std::string("x\xff\x80", 3), collation);
  }
}

void check_grouping(unsigned seed)
{
  using LockedMap = hash::ObHashMap<ObFTWord, int64_t>;
  ObFTWordMap actual;
  LockedMap reference;
  CHECK(actual.create(17, "FTSMapTest") == OB_SUCCESS);
  CHECK(reference.create(17, "FTSMapRef") == OB_SUCCESS);
  const std::vector<std::string> words = {
    "database", "DATABASE", "Database", "caféx", "cafex", "CAFEX", "中文", "全文", "audit", "performance"};
  std::mt19937 random(seed);
  for (int round = 0; round < 100; ++round) {
    for (int i = 0; i < 500; ++i) {
      const auto &text = words[random() % words.size()];
      const ObFTWord word(text.size(), text.data(), word_meta(CS_TYPE_UTF8MB4_GENERAL_CI));
      const int64_t increment = 1 + random() % 4;
      auto update = [&word, increment](hash::HashMapPair<ObFTWord, int64_t> &entry) {
        entry.first = word;
        entry.second += increment;
      };
      CHECK(actual.set_or_update(word, 1, update) == OB_SUCCESS);
      CHECK(reference.set_or_update(word, 1, update) == OB_SUCCESS);
    }
    CHECK(actual.size() == reference.size());
    auto expected = reference.begin();
    for (auto entry = actual.begin(); entry != actual.end(); ++entry, ++expected) {
      CHECK(expected != reference.end());
      // Compare raw spelling and iteration order, not collation equality alone.
      CHECK(entry->first.get_word().get_string() == expected->first.get_word().get_string());
      CHECK(entry->second == expected->second);
    }
    CHECK(expected == reference.end());
    actual.reuse();
    reference.reuse();
  }
  actual.destroy();
  reference.destroy();
}

template<typename Map>
void benchmark_grouping(const char *label)
{
  Map words;
  CHECK(words.create(127, "FTSMapBench") == OB_SUCCESS);
  const std::vector<std::string> text = {"database", "DATABASE", "audit", "performance", "caféx", "cafex", "中文"};
  const ObObjMeta meta = word_meta(CS_TYPE_UTF8MB4_GENERAL_CI);
  uint64_t checksum = 0;
  constexpr int rounds = 1000, tokens = 256;
  const auto start = std::chrono::steady_clock::now();
  for (int round = 0; round < rounds; ++round) {
    for (int i = 0; i < tokens; ++i) {
      const auto &value = text[i % text.size()];
      const ObFTWord word(value.size(), value.data(), meta);
      auto update = [&word](hash::HashMapPair<ObFTWord, int64_t> &entry) {
        entry.first = word;
        ++entry.second;
      };
      CHECK(words.set_or_update(word, 1, update) == OB_SUCCESS);
    }
    for (auto entry = words.begin(); entry != words.end(); ++entry) { checksum += entry->second; }
    words.reuse();
  }
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
  std::printf("grouping_%s_ns=%.3f checksum=%llu\n", label, ns / (rounds * tokens),
      static_cast<unsigned long long>(checksum));
  words.destroy();
}

void benchmark_stopwords(ObStopWordChecker &checker, ReferenceStopwords &reference)
{
  const std::vector<std::string> text = {"the", "database", "全文索引", "with", "audit", "thé", "performance", "tokenizer"};
  const ObObjMeta meta = word_meta(CS_TYPE_UTF8MB4_GENERAL_CI);
  constexpr int rounds = 100000;
  for (bool optimized : {false, true}) {
    uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < rounds; ++round) {
      for (const auto &value : text) {
        const ObFTWord word(value.size(), value.data(), meta);
        bool stop = false;
        CHECK((optimized ? checker.check_stopword(word, stop) : reference.check(word, stop)) == OB_SUCCESS);
        checksum += stop;
      }
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("stopword_%s_ns=%.3f checksum=%llu\n", optimized ? "optimized" : "original",
        ns / (rounds * text.size()), static_cast<unsigned long long>(checksum));
  }
}
}

int main(int argc, char **argv)
{
  CHECK(ObCharset::init_charset() == OB_SUCCESS);
  ObStopWordChecker checker;
  bool is_stopword = false;
  CHECK(checker.check_stopword(ObFTWord(), is_stopword) == OB_NOT_INIT);
  CHECK(checker.init() == OB_SUCCESS);
  CHECK(checker.init() == OB_INIT_TWICE);
  CHECK(checker.check_stopword(ObFTWord(), is_stopword) == OB_INVALID_ARGUMENT);
  ReferenceStopwords reference;
  if (argc > 1 && std::string(argv[1]) == "--bench") {
    benchmark_stopwords(checker, reference);
    benchmark_grouping<hash::ObHashMap<ObFTWord, int64_t>>("locked");
    benchmark_grouping<ObFTWordMap>("local");
    return 0;
  }
  check_stopwords(checker, reference);
  check_grouping(1);
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&, worker] {
      check_grouping(worker + 100);
      check_stopwords(checker, reference);
    });
  }
  for (auto &worker : workers) { worker.join(); }
  checker.destroy();
  CHECK(checker.check_stopword(ObFTWord(), is_stopword) == OB_NOT_INIT);
  std::puts("FTS stopword conversion, collation, grouping and concurrent-worker regressions passed");
}
