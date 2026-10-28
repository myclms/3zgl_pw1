// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include "lib/allocator/page_arena.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "share/text_analysis/ob_text_analyzer.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::storage;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); \
} } while (false)

namespace
{
using CharType = ObFTCharUtil::CharType;
uint64_t beng_digest = 14695981039346656037ULL;

void digest_bytes(const char *data, size_t size)
{
  for (size_t i = 0; i < size; ++i) {
    beng_digest = (beng_digest ^ static_cast<unsigned char>(data[i])) * 1099511628211ULL;
  }
}

// Keep the original predicate order as a differential oracle, including the
// existing decoder's behavior for UTF16 and malformed/truncated characters.
template<ObCharsetType CS>
void check_character(const char *bytes, uint8_t length, ObCollationType collation)
{
  const unsigned char *begin = reinterpret_cast<const unsigned char *>(bytes);
  ob_wc_t unicode = 0;
  const int decoded = ob_charset_decode_unicode<CS>(begin, begin + length, unicode);
  CharType expected = CharType::USELESS;
  if (decoded >= 0) {
    if (ObUnicodeBlockUtils::is_alpha(unicode)) { expected = CharType::ENGLISH_LETTER; }
    else if (ObUnicodeBlockUtils::is_arabic(unicode)) { expected = CharType::ARABIC_LETTER; }
    else if (ObUnicodeBlockUtils::is_chinese(unicode)) { expected = CharType::CHINESE; }
    else if (ObUnicodeBlockUtils::is_other_cjk(unicode)) { expected = CharType::OTHER_CJK; }
    else if (CS == CHARSET_UTF16 || CS == CHARSET_UTF16LE) {
      if (ObUnicodeBlockUtils::check_high_surrogate(unicode)) { expected = CharType::SURROGATE_HIGH; }
      else if (ObUnicodeBlockUtils::check_low_surrogate(unicode)) { expected = CharType::SURROGATE_LOW; }
    }
  }
  CharType actual = CharType::CHINESE;
  const int ret = ObFTCharUtil::classify_first_char(collation, bytes, length, actual);
  if (ret != (decoded < 0 ? OB_UNEXPECT_INTERNAL_ERROR : OB_SUCCESS) || actual != expected) {
    std::fprintf(stderr, "charset=%d collation=%d length=%u unicode=%lu decoded=%d ret=%d actual=%d expected=%d\n",
                 CS, collation, length, static_cast<unsigned long>(unicode), decoded, ret,
                 static_cast<int>(actual), static_cast<int>(expected));
  }
  CHECK(ret == (decoded < 0 ? OB_UNEXPECT_INTERNAL_ERROR : OB_SUCCESS));
  CHECK(actual == expected);
}

void check_classification()
{
  // All Unicode scalar values, rather than just the benchmark's Chinese text.
  unsigned char bytes[4];
  for (ob_wc_t cp = 0; cp <= 0x10ffff; ++cp) {
    if (cp >= 0xd800 && cp <= 0xdfff) { continue; }
    const int size = ob_charset_encode_unicode<CHARSET_UTF8MB4>(cp, bytes, bytes + sizeof(bytes));
    CHECK(size > 0);
    check_character<CHARSET_UTF8MB4>(reinterpret_cast<char *>(bytes), size, CS_TYPE_UTF8MB4_GENERAL_CI);
  }
  for (unsigned cp = 0; cp <= 0xffff; ++cp) {
    const char big_endian[] = {static_cast<char>(cp >> 8), static_cast<char>(cp)};
    const char little_endian[] = {static_cast<char>(cp), static_cast<char>(cp >> 8)};
    if (ObCharset::charset_type_by_coll(CS_TYPE_UTF16_GENERAL_CI) == CHARSET_UTF16) {
      check_character<CHARSET_UTF16>(big_endian, 2, CS_TYPE_UTF16_GENERAL_CI);
    }
    if (ObCharset::charset_type_by_coll(CS_TYPE_UTF16LE_GENERAL_CI) == CHARSET_UTF16LE) {
      check_character<CHARSET_UTF16LE>(little_endian, 2, CS_TYPE_UTF16LE_GENERAL_CI);
    }
  }
  const std::vector<std::string> invalid = {
    "", "\x80", "\xc0\x80", "\xc2", "\xe4\xb8", "\xed\xa0\x80", "\xf4\x90\x80\x80"};
  for (const auto &input : invalid) {
    check_character<CHARSET_UTF8MB4>(input.data(), input.size(), CS_TYPE_UTF8MB4_GENERAL_CI);
  }
}

void compare_beng(ObBEngFTParser &parser, ObIAllocator &allocator,
                  const std::string &input, ObCollationType collation)
{
  CHECK(!input.empty());
  ObFTParserParam param;
  param.cs_ = ObCharset::get_charset(collation);
  param.fulltext_ = input.data();
  param.ft_length_ = input.size();
  param.allocator_ = &allocator;
  CHECK(parser.init(&param) == OB_SUCCESS);
  CHECK(parser.init(&param) == OB_INIT_TWICE);

  ObTextAnalysisCtx context;
  context.cs_ = param.cs_;
  context.filter_stopword_ = false;
  context.need_grouping_ = false;
  ObEnglishTextAnalyzer reference;
  CHECK(reference.init(context, allocator) == OB_SUCCESS);
  ObDatum document;
  document.set_string(input.data(), input.size());
  ObITokenStream *stream = nullptr;
  CHECK(reference.analyze(document, stream) == OB_SUCCESS);

  std::vector<ObString> retained;
  std::vector<std::string> copied;
  for (;;) {
    const char *word = nullptr;
    int64_t length = 0, chars = 0, frequency = 0, expected_frequency = 0;
    ObDatum expected;
    int expected_ret = stream->get_next(expected, expected_frequency);
    if (expected_ret == OB_SUCCESS &&
        (expected.ptr_ == nullptr || expected.len_ == 0 || expected_frequency <= 0)) {
      expected_ret = OB_INVALID_ARGUMENT;
    }
    const int actual_ret = parser.get_next_token(word, length, chars, frequency);
    CHECK(actual_ret == expected_ret);
    const std::string status = std::to_string(actual_ret) + ":" + std::to_string(frequency) + ":";
    digest_bytes(status.data(), status.size());
    if (actual_ret != OB_SUCCESS) { break; }
    CHECK(length == expected.len_ && chars == length && frequency == expected_frequency);
    CHECK(std::string(word, length) == std::string(expected.ptr_, expected.len_));
    retained.emplace_back(length, word);
    copied.emplace_back(word, length);
    digest_bytes(word, length);
    digest_bytes(";", 1);
  }
  parser.reset();
  // Word-map keys must remain valid after advancing and destroying the parser
  // pipeline; they belong to the caller's allocator.
  for (size_t i = 0; i < copied.size(); ++i) {
    CHECK(std::string(retained[i].ptr(), retained[i].length()) == copied[i]);
  }
  reference.reset();
}

void check_beng()
{
  ObArenaAllocator allocator("FTSHotPathTest");
  ObBEngFTParser parser(allocator);
  const std::vector<std::string> documents = {
    "The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path.",
    "Database DATABASE database performance audit!",
    "  \t\n___ -- !!! 123 abc-DEF foo@example.com @ bar ",
    "中文 OceanBase 全文索引 ＡＢＣ１２３ café résumé database",
    std::string("A\0B c\x80 d\xe4\xb8", 10), "a", "!", "\xff\xfe"};
  const ObCollationType collations[] = {
    CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN, CS_TYPE_LATIN1_SWEDISH_CI,
    CS_TYPE_GBK_CHINESE_CI, CS_TYPE_UTF16_GENERAL_CI, CS_TYPE_UTF16LE_GENERAL_CI};
  for (auto collation : collations) {
    if (ObCharset::get_charset(collation) == nullptr) { continue; }
    for (const auto &document : documents) {
      compare_beng(parser, allocator, document, collation);
      allocator.reuse();
    }
  }
  std::mt19937 random(20261028);
  for (int i = 0; i < 1000; ++i) {
    std::string document(1 + random() % 256, ' ');
    for (char &c : document) { c = static_cast<char>(random() % 256); }
    compare_beng(parser, allocator, document, CS_TYPE_UTF8MB4_GENERAL_CI);
    allocator.reuse();
  }
  ObFTParserParam invalid;
  CHECK(parser.init(nullptr) == OB_INVALID_ARGUMENT);
  CHECK(parser.init(&invalid) == OB_INVALID_ARGUMENT);
}

void benchmark_beng()
{
  const std::string text = "The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path.";
  ObArenaAllocator allocator("FTSHotPathTest");
  uint64_t checksum = 0;
  constexpr int rounds = 30000;
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < rounds; ++i) {
    ObBEngFTParser parser(allocator);
    ObFTParserParam param;
    param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI);
    param.fulltext_ = text.data();
    param.ft_length_ = text.size();
    param.allocator_ = &allocator;
    CHECK(parser.init(&param) == OB_SUCCESS);
    const char *word = nullptr;
    int64_t length = 0, chars = 0, frequency = 0;
    int ret;
    while ((ret = parser.get_next_token(word, length, chars, frequency)) == OB_SUCCESS) {
      checksum += length + frequency + static_cast<unsigned char>(word[0]);
    }
    CHECK(ret == OB_ITER_END);
    parser.reset();
    allocator.reuse();
  }
  const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("beng_document_us=%.3f rounds=%d checksum=%llu\n", elapsed * 1000 / rounds,
              rounds, static_cast<unsigned long long>(checksum));
}

void benchmark_classification()
{
  const std::string text = "OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低 CPU 开销。";
  std::vector<ObString> characters;
  for (size_t pos = 0; pos < text.size();) {
    const auto *begin = reinterpret_cast<const unsigned char *>(text.data() + pos);
    const auto *end = reinterpret_cast<const unsigned char *>(text.data() + text.size());
    const int length = ob_charset_char_len<CHARSET_UTF8MB4>(begin, end);
    CHECK(length > 0);
    characters.emplace_back(length, text.data() + pos);
    pos += length;
  }
  uint64_t checksum = 0;
  constexpr int rounds = 100000;
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < rounds; ++i) {
    for (const auto &character : characters) {
      CharType type;
      CHECK(ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_GENERAL_CI,
            character.ptr(), character.length(), type) == OB_SUCCESS);
      checksum += static_cast<uint8_t>(type);
    }
  }
  const double elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
  std::printf("ik_classify_ns=%.3f characters=%zu checksum=%llu\n", elapsed / (rounds * characters.size()),
              rounds * characters.size(), static_cast<unsigned long long>(checksum));
}
}

int main(int argc, char **argv)
{
  CHECK(ObCharset::init_charset() == OB_SUCCESS);
  if (argc > 1 && std::string(argv[1]) == "--bench") {
    benchmark_beng();
    benchmark_classification();
  } else {
    check_classification();
    check_beng();
    std::printf("beng_output_digest=%llu\n", static_cast<unsigned long long>(beng_digest));
    std::puts("FTS character classification and BENG differential regressions passed");
  }
  return 0;
}
