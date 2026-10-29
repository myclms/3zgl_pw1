// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "storage/fts/dict/ob_ft_dat_dict.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); \
} } while (false)

namespace
{
ObString view(const std::string &word)
{
  return ObString(word.size(), word.data());
}

void check_map(size_t word_count)
{
  const size_t capacity = ObArrayHashMap::estimate_capacity(word_count);
  CHECK(capacity >= 128 && (capacity & (capacity - 1)) == 0);
  CHECK(capacity >= word_count * 4 / 3);
  // The dictionary is packed in a cache block; exercise an unaligned address.
  std::vector<char> storage(ObArrayHashMap::estimate_size(word_count) + 1);
  auto *map = reinterpret_cast<ObArrayHashMap *>(storage.data() + 1);
  CHECK(map->init(word_count) == OB_SUCCESS);
  CHECK(map->capacity() == capacity);
  std::unordered_map<std::string, int> expected;
  std::mt19937 random(20261029);
  for (size_t i = 0; i < word_count;) {
    const size_t length = 1 + random() % ObCharset::MAX_MB_LEN;
    std::string word(length, '\0');
    for (char &byte : word) { byte = static_cast<char>(random()); }
    if (expected.emplace(word, i + 1).second) {
      CHECK(map->insert(view(word), ++i) == OB_SUCCESS);
    }
  }
  for (const auto &entry : expected) {
    ObFTSingleWord packed;
    CHECK(packed.set_word(entry.first.data(), entry.first.size()) == OB_SUCCESS);
    int code = -1;
    CHECK(map->find(packed, code) == OB_SUCCESS && code == entry.second);
    code = -1;
    CHECK(map->find(view(entry.first), code) == OB_SUCCESS && code == entry.second);
  }
  for (int i = 0; i < 2000; ++i) {
    std::string word(ObCharset::MAX_MB_LEN, '\0');
    for (char &byte : word) { byte = static_cast<char>(random()); }
    const auto found = expected.find(word);
    int code = -1;
    const int ret = map->find(view(word), code);
    if (found == expected.end()) {
      CHECK(ret == OB_ENTRY_NOT_EXIST && code == -1);
    } else {
      CHECK(ret == OB_SUCCESS && code == found->second);
    }
  }
}

void check_collision_wraparound()
{
  std::vector<char> storage(ObArrayHashMap::estimate_size(80));
  auto *map = reinterpret_cast<ObArrayHashMap *>(storage.data());
  CHECK(map->init(80) == OB_SUCCESS);
  std::vector<std::string> words;
  for (uint32_t value = 0; words.size() < 90; ++value) {
    std::string word(sizeof(value), '\0');
    std::memcpy(word.data(), &value, sizeof(value));
    if ((view(word).hash() & (map->capacity() - 1)) == map->capacity() - 1) {
      words.push_back(word);
    }
  }
  // An occupied probe chain wraps repeatedly around the end of the table.
  for (size_t i = 0; i < 89; ++i) {
    CHECK(map->insert(view(words[i]), i + 1) == OB_SUCCESS);
  }
  for (size_t i = 0; i < words.size(); ++i) {
    int code = -1;
    CHECK(map->find(view(words[i]), code) == (i == 89 ? OB_ENTRY_NOT_EXIST : OB_SUCCESS));
    CHECK(code == (i == 89 ? -1 : static_cast<int>(i + 1)));
  }
}

// The original lookup, including the caller's character copy, for a native
// before/after measurement independent of SQL/network overhead.
int reference_lookup(const ObArrayHashMap::Header &header, const ObString &input, int &code)
{
  ObFTSingleWord word;
  CHECK(word.set_word(input.ptr(), input.length()) == OB_SUCCESS);
  size_t index = word.get_word().hash() % header.capacity_;
  while (header.data[index].used) {
    if (header.data[index].word == word) {
      code = header.data[index].code;
      return OB_SUCCESS;
    }
    index = (index + 3) % header.capacity_;
  }
  return OB_ENTRY_NOT_EXIST;
}

void benchmark()
{
  constexpr size_t count = 2000;
  std::vector<std::string> words;
  std::vector<char> storage(ObArrayHashMap::estimate_size(count));
  auto *map = reinterpret_cast<ObArrayHashMap *>(storage.data());
  CHECK(map->init(count) == OB_SUCCESS);
  // UTF8 three-byte characters, as consumed by IK's dictionary reader.
  for (size_t i = 0; i < count; ++i) {
    const uint32_t cp = 0x4e00 + i;
    words.push_back(std::string{static_cast<char>(0xe0 | (cp >> 12)),
        static_cast<char>(0x80 | ((cp >> 6) & 63)), static_cast<char>(0x80 | (cp & 63))});
    CHECK(map->insert(view(words.back()), i + 1) == OB_SUCCESS);
  }
  // Same entries and load factor for both algorithms, so this does not ascribe
  // gains from a larger table to the lookup itself.
  const auto *header = reinterpret_cast<const ObArrayHashMap::Header *>(map);
  for (int sample = 0; sample < 6; ++sample) {
    for (bool reference : {sample % 2 == 0, sample % 2 != 0}) {
      uint64_t checksum = 0;
      const auto start = std::chrono::steady_clock::now();
      for (int round = 0; round < 2000; ++round) {
        for (const std::string &word : words) {
          int code = 0;
          const int ret = reference ? reference_lookup(*header, view(word), code)
                                    : map->find(view(word), code);
          CHECK(ret == OB_SUCCESS);
          checksum += code;
        }
      }
      const double ns = std::chrono::duration<double, std::nano>(
          std::chrono::steady_clock::now() - start).count() / (2000 * count);
      std::printf("%s_ns=%.3f checksum=%llu\n", reference ? "reference" : "candidate",
          ns, static_cast<unsigned long long>(checksum));
    }
  }
}
}

int main(int argc, char **argv)
{
  CHECK(ObCharset::init_charset() == OB_SUCCESS);
  for (size_t count : {0, 1, 75, 80, 95, 96, 97, 100, 383, 384, 385, 2000}) {
    check_map(count);
  }
  check_collision_wraparound();
  if (argc > 1 && std::string(argv[1]) == "--bench") { benchmark(); }
  std::puts("FTS dictionary lookup regressions passed");
}
