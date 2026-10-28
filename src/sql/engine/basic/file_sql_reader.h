// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <climits>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <rapidjson/document.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {
static constexpr const char *MARKER = "__seekdb_file_sql_v1__";
static constexpr size_t MAX_RECORD = 16 * 1024 * 1024;
static constexpr size_t MAX_COLUMNS = 1024;
enum class Kind { CSV, JSONL, PARQUET, GLOB };
enum class Type { NIL, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
struct Column {
  std::string name;
  std::string key;
  Type type = Type::NIL;
  bool nullable = false;
  unsigned datetime_scale = 0;
};
struct Cell { std::string text; bool null = false; bool quoted = false; Type declared = Type::NIL; };
struct Options {
  Kind kind = Kind::CSV;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string nullstr;
};
inline std::string lower(std::string s) {
  for (char &c : s) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
  return s;
}
inline bool utf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i++];
    if (c < 128) { continue; }
    unsigned n = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
    if (!n || i + n > s.size()) { return false; }
    unsigned char first = s[i];
    if ((c == 0xE0 && first < 0xA0) || (c == 0xED && first >= 0xA0)
        || (c == 0xF0 && first < 0x90) || (c == 0xF4 && first >= 0x90)) { return false; }
    while (n--) { if ((static_cast<unsigned char>(s[i++]) & 0xC0) != 0x80) { return false; } }
  }
  return true;
}
inline std::string json_string(const std::string &s) {
  const char *hex = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else { out += c; }
  }
  return out + '"';
}
inline bool date_valid(const std::string &s, bool datetime) {
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') { return false; }
  const int positions[] = {0,1,2,3,5,6,8,9};
  for (int i : positions) { if (s[i] < '0' || s[i] > '9') { return false; } }
  int y = std::atoi(s.substr(0,4).c_str()), m = std::atoi(s.substr(5,2).c_str()), d = std::atoi(s.substr(8,2).c_str());
  int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  if (y < 1 || m < 1 || m > 12) { return false; }
  days[2] += y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  if (d < 1 || d > days[m]) { return false; }
  if (!datetime) { return s.size() == 10; }
  if (s.size() < 19 || (s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':') { return false; }
  for (int i : {11,12,14,15,17,18}) { if (s[i] < '0' || s[i] > '9') { return false; } }
  if (std::atoi(s.substr(11,2).c_str()) > 23 || std::atoi(s.substr(14,2).c_str()) > 59
      || std::atoi(s.substr(17,2).c_str()) > 59) { return false; }
  if (s.size() > 19) {
    if (s[19] != '.' || s.size() < 21 || s.size() > 26) { return false; }
    for (size_t i = 20; i < s.size(); ++i) { if (s[i] < '0' || s[i] > '9') { return false; } }
  }
  return true;
}
inline Type infer(const Cell &cell) {
  if (cell.null) { return Type::NIL; }
  const std::string &s = cell.text;
  if (cell.declared != Type::NIL) {
    if (cell.declared == Type::VARCHAR && date_valid(s,false)) { return Type::DATE; }
    if (cell.declared == Type::VARCHAR && date_valid(s,true)) { return Type::DATETIME; }
    return cell.declared;
  }
  if (s.empty()) { return Type::VARCHAR; }
  if (lower(s) == "true" || lower(s) == "false") { return Type::BOOLEAN; }
  char *end = nullptr; errno = 0;
  std::strtoll(s.c_str(), &end, 10);
  if (errno == 0 && end == s.c_str() + s.size() && end != s.c_str()
      && s.front() != ' ' && s.front() != '\t') { return Type::BIGINT; }
  // Integers outside signed 64 bits remain exact strings, rather than losing precision.
  bool integer = true;
  for (size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') { integer = false; }
  }
  if (integer) { return Type::VARCHAR; }
  errno = 0; double number = std::strtod(s.c_str(), &end);
  if (errno == 0 && end == s.c_str() + s.size() && end != s.c_str()
      && std::isfinite(number) && s.front() != ' ' && s.front() != '\t') { return Type::DOUBLE; }
  if (date_valid(s, false)) { return Type::DATE; }
  if (date_valid(s, true)) { return Type::DATETIME; }
  return Type::VARCHAR;
}
inline Type widen(Type a, Type b) {
  if (a == b || b == Type::NIL) { return a; }
  if (a == Type::NIL) { return b; }
  if ((a == Type::BIGINT || a == Type::DOUBLE || a == Type::BOOLEAN)
      && (b == Type::BIGINT || b == Type::DOUBLE || b == Type::BOOLEAN)) {
    return a == Type::DOUBLE || b == Type::DOUBLE ? Type::DOUBLE : Type::BIGINT;
  }
  if ((a == Type::DATE && b == Type::DATETIME) || (a == Type::DATETIME && b == Type::DATE)) { return Type::DATETIME; }
  return Type::VARCHAR;
}
inline const char *sql_type(Type t) {
  switch (t) {
    case Type::BIGINT: return "BIGINT";
    case Type::DOUBLE: return "DOUBLE";
    case Type::BOOLEAN: return "BOOLEAN";
    case Type::DATE: return "DATE";
    case Type::DATETIME: return "DATETIME(6)";
    default: return "VARCHAR(65535)";
  }
}
inline std::string sql_type(const Column &column) {
  if (column.type == Type::DATETIME) {
    return column.datetime_scale == 0 ? "DATETIME"
        : "DATETIME(" + std::to_string(column.datetime_scale) + ")";
  }
  return sql_type(column.type);
}
inline bool canonical(const std::string &s, std::string &out) {
  if (s.empty() || s.find('\0') != std::string::npos || s.find("://") != std::string::npos) { return false; }
  char buffer[4096];
#ifdef _WIN32
  if (!_fullpath(buffer, s.c_str(), sizeof(buffer))) { return false; }
#else
  if (!realpath(s.c_str(), buffer)) { return false; }
#endif
  out = buffer; return true;
}
inline bool is_directory_mode(unsigned mode) {
#ifdef _WIN32
  return (mode & _S_IFMT) == _S_IFDIR;
#else
  return S_ISDIR(mode);
#endif
}
inline bool is_regular_mode(unsigned mode) {
#ifdef _WIN32
  return (mode & _S_IFMT) == _S_IFREG;
#else
  return S_ISREG(mode);
#endif
}
inline bool allowed(const std::string &path, const std::string &secure) {
  std::string root;
  if (!canonical(secure, root)) { return false; }
  struct stat st{};
  if (stat(root.c_str(), &st) || !is_directory_mode(st.st_mode)) { return false; }
  while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) { root.pop_back(); }
#ifdef _WIN32
  std::string a = lower(path), b = lower(root);
#else
  const std::string &a = path, &b = root;
#endif
  return b == "/" || (a.size() > b.size() && a.compare(0,b.size(),b) == 0 && (a[b.size()] == '/' || a[b.size()] == '\\'));
}
inline std::string schema_signature(const std::vector<Column> &columns) {
  std::string value;
  for (const auto &col : columns) {
    value += json_string(col.name) + json_string(col.key)
        + std::to_string(static_cast<int>(col.type)) + ":"
        + std::to_string(col.datetime_scale) + (col.nullable ? "?;" : "!;");
  }
  return value;
}
inline std::string encode_config(const std::string &path, const Options &options,
    const std::string &fingerprint, const std::vector<Column> &columns) {
  return std::string(MARKER) + "{\"path\":" + json_string(path)
      + ",\"kind\":" + std::to_string(static_cast<int>(options.kind))
      + ",\"header\":" + (options.header ? "true" : "false")
      + ",\"delimiter\":" + std::to_string(static_cast<unsigned char>(options.delimiter))
      + ",\"quote\":" + std::to_string(static_cast<unsigned char>(options.quote))
      + ",\"escape\":" + std::to_string(static_cast<unsigned char>(options.escape))
      + ",\"nullstr\":" + json_string(options.nullstr)
      + ",\"identity\":" + json_string(fingerprint)
      + ",\"schema\":" + json_string(schema_signature(columns)) + "}";
}
inline bool decode_config(const std::string &value, std::string &path, Options &options,
    std::string &fingerprint, std::string &schema) {
  rapidjson::Document d;
  if (value.compare(0, std::strlen(MARKER), MARKER) != 0) { return false; }
  d.Parse(value.c_str() + std::strlen(MARKER));
  if (d.HasParseError() || !d.IsObject()) { return false; }
  for (const char *name : {"path", "nullstr", "identity", "schema"}) {
    if (!d.HasMember(name) || !d[name].IsString()) { return false; }
  }
  for (const char *name : {"kind", "delimiter", "quote", "escape"}) {
    if (!d.HasMember(name) || !d[name].IsInt()) { return false; }
  }
  if (!d.HasMember("header") || !d["header"].IsBool() || d["kind"].GetInt() < 0 || d["kind"].GetInt() > 3) { return false; }
  path.assign(d["path"].GetString(),d["path"].GetStringLength());
  options.nullstr.assign(d["nullstr"].GetString(),d["nullstr"].GetStringLength());
  options.kind = static_cast<Kind>(d["kind"].GetInt()); options.header = d["header"].GetBool();
  options.delimiter = d["delimiter"].GetInt(); options.quote = d["quote"].GetInt(); options.escape = d["escape"].GetInt();
  if (options.delimiter == 0 || options.quote == 0 || options.escape == 0 || options.delimiter == options.quote) { return false; }
  fingerprint = d["identity"].GetString(); schema = d["schema"].GetString(); return true;
}

// Format: https://github.com/apache/parquet-format/blob/master/src/main/thrift/parquet.thrift
// Encodings: https://parquet.apache.org/docs/file-format/data-pages/encodings/
// Minimal flat Parquet reader: Compact Thrift metadata, PLAIN/dictionary data
// pages, RLE/bit-packed definition levels and raw Snappy. Only page buffers and
// dictionaries are retained; all relational work stays in the seekdb engine.
struct CompactValue {
  int64_t number = 0;
  std::string bytes;
  std::map<int, CompactValue> fields;
  std::vector<CompactValue> list;
  const CompactValue &field(int id) const {
    static const CompactValue absent;
    auto it = fields.find(id); return it == fields.end() ? absent : it->second;
  }
  bool has(int id) const { return fields.find(id) != fields.end(); }
};
class CompactReader {
public:
  CompactReader(const char *data, size_t length) : data_(reinterpret_cast<const uint8_t *>(data)), size_(length) {}
  bool structure(CompactValue &out, unsigned depth = 0) {
    if (depth > 32) { return false; }
    int previous = 0;
    for (unsigned count = 0; count < 4096; ++count) {
      uint8_t header;
      if (!byte(header)) { return false; }
      if (!header) { return true; }
      uint64_t id;
      if (header >> 4) { id = previous + (header >> 4); }
      else { uint64_t encoded; if (!varint(encoded)) { return false; } id = zigzag(encoded); }
      if (id > 32767 || out.fields.count(id)) { return false; }
      previous = id;
      CompactValue value;
      if (!read(header & 15, value, depth + 1, true)) { return false; }
      out.fields.emplace(id,std::move(value));
    }
    return false;
  }
  size_t position() const { return pos_; }
private:
  bool byte(uint8_t &b) { if (pos_ == size_) { return false; } b = data_[pos_++]; return true; }
  bool varint(uint64_t &v) {
    v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      uint8_t b; if (!byte(b) || (shift == 63 && b > 1)) { return false; }
      v |= uint64_t(b & 127) << shift;
      if (!(b & 128)) { return true; }
    }
    return false;
  }
  static int64_t zigzag(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }
  bool read(unsigned type, CompactValue &out, unsigned depth, bool field) {
    uint64_t n; uint8_t b;
    if (depth > 32 || ++nodes_ > 131072) { return false; }
    switch (type) {
      case 1: case 2:
        if (!field && (!byte(b) || (b != 1 && b != 2))) { return false; }
        out.number = field ? type == 1 : b == 1; return true;
      case 3: if (!byte(b)) { return false; } out.number = static_cast<int8_t>(b); return true;
      case 4: case 5: case 6:
        if (!varint(n)) { return false; } out.number = zigzag(n); return true;
      case 7:
        if (size_ - pos_ < 8) { return false; } pos_ += 8; return true;
      case 8:
        if (!varint(n) || n > MAX_RECORD || n > size_ - pos_) { return false; }
        out.bytes.assign(reinterpret_cast<const char *>(data_ + pos_),n); pos_ += n; return true;
      case 9: case 10:
        if (!byte(b)) { return false; } n = b >> 4;
        if (n == 15 && !varint(n)) { return false; }
        if (n > 1000000) { return false; }
        for (uint64_t i = 0; i < n; ++i) {
          CompactValue item;
          if (!read(b & 15,item,depth + 1,false)) { return false; }
          out.list.push_back(std::move(item));
        }
        return true;
      case 11:
        if (!varint(n) || n > 1000000) { return false; }
        if (!n) { return true; }
        if (!byte(b)) { return false; }
        for (uint64_t i = 0; i < n; ++i) {
          CompactValue key, value;
          if (!read(b >> 4,key,depth + 1,false) || !read(b & 15,value,depth + 1,false)) { return false; }
        }
        return true;
      case 12: return structure(out,depth + 1);
      default: return false;
    }
  }
  const uint8_t *data_; size_t size_, pos_ = 0, nodes_ = 0;
};
inline uint64_t little(const char *p, unsigned bytes) {
  uint64_t v = 0;
  for (unsigned i = 0; i < bytes; ++i) { v |= uint64_t(static_cast<unsigned char>(p[i])) << (i * 8); }
  return v;
}
inline bool parquet_varint(const std::string &s, size_t &pos, uint64_t &v) {
  v = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    if (pos == s.size()) { return false; }
    unsigned char c = s[pos++]; if (shift == 63 && c > 1) { return false; }
    v |= uint64_t(c & 127) << shift;
    if (!(c & 128)) { return true; }
  }
  return false;
}
inline bool snappy_decode(const std::string &input, size_t expected, std::string &output) {
  output.clear(); size_t pos = 0; uint64_t length;
  if (expected > MAX_RECORD || !parquet_varint(input,pos,length) || length != expected) { return false; }
  output.reserve(expected);
  while (pos < input.size() && output.size() < expected) {
    unsigned char tag = input[pos++]; unsigned type = tag & 3;
    uint64_t count, offset;
    if (type == 0) {
      count = tag >> 2;
      if (count < 60) { ++count; }
      else {
        unsigned n = count - 59;
        if (n > 4 || n > input.size() - pos) { return false; }
        count = little(input.data() + pos,n) + 1; pos += n;
      }
      if (count > expected - output.size() || count > input.size() - pos) { return false; }
      output.append(input,pos,count); pos += count;
    } else {
      unsigned n = type == 1 ? 1 : type == 2 ? 2 : 4;
      if (n > input.size() - pos) { return false; }
      offset = little(input.data() + pos,n); pos += n;
      if (type == 1) { count = 4 + ((tag >> 2) & 7); offset |= uint64_t(tag & 0xE0) << 3; }
      else { count = 1 + (tag >> 2); }
      if (offset == 0 || offset > output.size() || count > expected - output.size()) { return false; }
      while (count--) { output += output[output.size() - offset]; }
    }
  }
  return pos == input.size() && output.size() == expected;
}
inline bool hybrid_decode(const std::string &input, size_t begin, size_t length,
    unsigned bits, size_t count, std::vector<uint32_t> &output) {
  output.clear();
  if (bits > 32 || begin > input.size() || length > input.size() - begin || count > MAX_RECORD) { return false; }
  std::string data = input.substr(begin,length); size_t pos = 0;
  while (output.size() < count) {
    uint64_t header; if (!parquet_varint(data,pos,header) || !header) { return false; }
    uint64_t n = header >> 1;
    if (!(header & 1)) {
      unsigned bytes = (bits + 7) / 8;
      if (bytes > data.size() - pos) { return false; }
      uint32_t value = little(data.data() + pos,bytes); pos += bytes;
      if (bits < 32 && value >= (uint64_t(1) << bits)) { return false; }
      n = std::min<uint64_t>(n,count - output.size());
      output.insert(output.end(),n,value);
    } else {
      if (n > MAX_RECORD / 8) { return false; }
      n *= 8; uint64_t bytes = (n * bits + 7) / 8;
      if (bytes > data.size() - pos) { return false; }
      for (uint64_t i = 0; i < n && output.size() < count; ++i) {
        uint32_t value = 0;
        for (unsigned j = 0; j < bits; ++j) {
          size_t bit = i * bits + j;
          value |= uint32_t((static_cast<unsigned char>(data[pos + bit / 8]) >> (bit % 8)) & 1) << j;
        }
        output.push_back(value);
      }
      pos += bytes;
    }
  }
  return true;
}
inline std::string parquet_date(int64_t days) {
  days += 719468;
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  unsigned doe = days - era * 146097;
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t year = yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  unsigned day = doy - (153 * mp + 2) / 5 + 1, month = mp + (mp < 10 ? 3 : -9);
  year += month <= 2;
  char text[64]; std::snprintf(text,sizeof(text),"%04lld-%02u-%02u",static_cast<long long>(year),month,day); return text;
}
class ParquetReader {
public:
  bool open(FILE *file, std::vector<Column> &columns) {
    file_ = file; struct stat st{};
    if (fstat(fileno(file),&st) || st.st_size < 12) { return fail("invalid Parquet length"); }
    file_size_ = st.st_size;
    std::string magic, end;
    if (!read_at(0,4,magic) || magic != "PAR1" || !read_at(file_size_ - 8,8,end) || end.substr(4) != "PAR1") { return fail("invalid Parquet magic"); }
    uint64_t length = little(end.data(),4);
    if (length > MAX_RECORD || length > file_size_ - 12) { return fail("Parquet footer exceeds resource limit"); }
    std::string footer;
    if (!read_at(file_size_ - 8 - length,length,footer)) { return false; }
    CompactReader parser(footer.data(),footer.size());
    if (!parser.structure(metadata_) || parser.position() != footer.size()) { return fail("invalid Compact Thrift footer"); }
    const auto &schema = metadata_.field(2).list;
    if (schema.size() < 2 || schema.size() > MAX_COLUMNS + 1 || schema[0].field(5).number != static_cast<int64_t>(schema.size() - 1)) { return fail("only flat Parquet schemas are supported"); }
    for (size_t i = 1; i < schema.size(); ++i) {
      const auto &col = schema[i];
      int physical = col.field(1).number, converted = col.has(6) ? col.field(6).number : -1;
      if (!col.has(1) || col.has(5) || col.field(3).number > 1 || physical < 0 || physical > 7) { return fail("nested or repeated Parquet column"); }
      Type type = physical == 0 ? Type::BOOLEAN : physical == 1 || physical == 2 ? Type::BIGINT
          : physical == 4 || physical == 5 ? Type::DOUBLE : Type::VARCHAR;
      if (converted == 6 || col.field(10).has(6)) { type = Type::DATE; converted = 6; }
      if (converted == 9 || converted == 10 || physical == 3 || col.field(10).has(8)) {
        type = Type::DATETIME;
        if (col.field(10).has(8)) {
          const auto &unit = col.field(10).field(8).field(2);
          converted = unit.has(1) ? 9 : unit.has(2) ? 10 : unit.has(3) ? 100 : -1;
          if (converted < 0) { return fail("unknown Parquet timestamp unit"); }
        }
      }
      if (converted == 5 || physical == 7) { return fail("Parquet decimal/fixed byte array is unsupported"); }
      types_.push_back(physical); converted_.push_back(converted); optional_.push_back(col.field(3).number == 1);
      columns.push_back({col.field(4).bytes,col.field(4).bytes,type,col.field(3).number == 1,
          type == Type::DATETIME ? (converted == 9 ? 3U : 6U) : 0U});
    }
    for (const auto &group : metadata_.field(4).list) {
      if (group.field(1).list.size() != columns.size() || group.field(3).number < 0) { return fail("invalid Parquet row group"); }
      for (const auto &chunk : group.field(1).list) { if (!chunk.field(1).bytes.empty()) { return fail("external Parquet column files are forbidden"); } }
    }
    rewind(); return true;
  }
  void set_projection(const std::vector<bool> &projection) { projection_ = projection; }
  void rewind() { group_ = 0; row_ = 0; cursors_.clear(); }
  int next(std::vector<Cell> &row) {
    const auto &groups = metadata_.field(4).list;
    while (group_ < groups.size() && row_ >= static_cast<uint64_t>(groups[group_].field(3).number)) { ++group_; row_ = 0; cursors_.clear(); }
    if (group_ == groups.size()) { return 1; }
    if (cursors_.empty()) {
      const auto &chunks = groups[group_].field(1).list;
      for (size_t i = 0; i < chunks.size(); ++i) {
        const auto &meta = chunks[i].field(3);
        Cursor cursor; cursor.codec = meta.field(4).number;
        cursor.offset = meta.has(11) ? std::min(meta.field(9).number,meta.field(11).number) : meta.field(9).number;
        cursor.physical = types_[i]; cursor.converted = converted_[i]; cursor.optional = optional_[i];
        if (meta.field(1).number != cursor.physical || cursor.offset < 4 || meta.field(5).number != groups[group_].field(3).number) { fail("invalid Parquet column metadata"); return -1; }
        if (cursor.codec != 0 && cursor.codec != 1) { fail("unsupported Parquet compression; use uncompressed or Snappy"); return -1; }
        cursors_.push_back(std::move(cursor));
      }
    }
    row.clear();
    for (size_t i = 0; i < cursors_.size(); ++i) {
      auto &cursor = cursors_[i];
      if (!projection_.empty() && (i >= projection_.size() || !projection_[i])) { row.push_back({"",true,false}); continue; }
      while (cursor.pos == cursor.values.size()) { if (!page(cursor)) { return -1; } }
      row.push_back(cursor.values[cursor.pos++]);
    }
    ++row_; return 0;
  }
  std::string error;
private:
  struct Cursor {
    uint64_t offset = 0; int codec = 0, physical = 0, converted = -1; bool optional = false;
    std::vector<Cell> dictionary, values; size_t pos = 0;
  };
  bool fail(const std::string &s) { error = s; return false; }
  bool read_at(uint64_t offset, size_t count, std::string &out) {
    if (offset > file_size_ || count > file_size_ - offset || count > MAX_RECORD) { return fail("Parquet page offset/length out of bounds"); }
    #ifdef _WIN32
    if (_fseeki64(file_,offset,SEEK_SET))
#else
    if (fseeko(file_,offset,SEEK_SET))
#endif
    { return fail("Parquet seek failed"); }
    out.resize(count);
    return std::fread(&out[0],1,count,file_) == count || fail("truncated Parquet page");
  }
  static std::string timestamp(int64_t day, int64_t rem) {
    char time[64]; std::snprintf(time,sizeof(time)," %02lld:%02lld:%02lld.%06lld",
        static_cast<long long>(rem / 3600000000LL),static_cast<long long>((rem / 60000000) % 60),
        static_cast<long long>((rem / 1000000) % 60),static_cast<long long>(rem % 1000000));
    return parquet_date(day) + time;
  }
  bool plain(const std::string &data, size_t &pos, size_t count, Cursor &cursor, std::vector<Cell> &values) {
    if (count > 262144) { return fail("Parquet value count exceeds limit"); }
    values.clear();
    for (size_t i = 0; i < count; ++i) {
      Cell value; unsigned bytes = cursor.physical == 1 || cursor.physical == 4 ? 4 : cursor.physical == 2 || cursor.physical == 5 ? 8 : cursor.physical == 3 ? 12 : 0;
      if (bytes > data.size() - pos) { return fail("truncated PLAIN value"); }
      if (cursor.physical == 0) {
        if (pos + i / 8 >= data.size()) { return fail("truncated boolean values"); }
        value.text = (static_cast<unsigned char>(data[pos + i / 8]) >> (i % 8)) & 1 ? "true" : "false";
      } else if (cursor.physical == 1 || cursor.physical == 2) {
        int64_t number = bytes == 4 ? static_cast<int32_t>(little(data.data() + pos,bytes)) : static_cast<int64_t>(little(data.data() + pos,bytes));
        value.text = std::to_string(number);
        if (cursor.converted == 6) { value.text = parquet_date(number); }
        else if (cursor.converted == 9 || cursor.converted == 10 || cursor.converted == 100) {
          if (cursor.converted == 9) { if (number > INT64_MAX / 1000 || number < INT64_MIN / 1000) { return fail("timestamp overflow"); } number *= 1000; }
          if (cursor.converted == 100) { number = number / 1000 - (number % 1000 < 0 ? 1 : 0); }
          int64_t day = number / 86400000000LL, rem = number % 86400000000LL;
          if (rem < 0) { rem += 86400000000LL; --day; }
          value.text = timestamp(day,rem);
        }
      } else if (cursor.physical == 3) {
        uint64_t nanos = little(data.data() + pos,8);
        int64_t day = static_cast<int64_t>(little(data.data() + pos + 8,4)) - 2440588;
        if (nanos >= 86400000000000ULL) { return fail("invalid INT96 time of day"); }
        value.text = timestamp(day,nanos / 1000);
      } else if (cursor.physical == 4 || cursor.physical == 5) {
        double number;
        if (bytes == 4) { uint32_t raw = little(data.data() + pos,4); float f; std::memcpy(&f,&raw,4); number = f; }
        else { uint64_t raw = little(data.data() + pos,8); std::memcpy(&number,&raw,8); }
        if (!std::isfinite(number)) { return fail("non-finite Parquet number"); }
        char text[64]; std::snprintf(text,sizeof(text),"%.17g",number); value.text = text;
      } else if (cursor.physical == 6) {
        if (data.size() - pos < 4) { return fail("truncated BYTE_ARRAY length"); }
        uint64_t length = little(data.data() + pos,4); pos += 4;
        if (length > data.size() - pos) { return fail("truncated BYTE_ARRAY"); }
        value.text = data.substr(pos,length); pos += length; value.quoted = true;
        if (!utf8(value.text)) { return fail("Parquet string is not UTF-8"); }
      } else { return fail("unsupported Parquet physical type"); }
      pos += bytes; values.push_back(std::move(value));
    }
    if (cursor.physical == 0) { pos += (count + 7) / 8; }
    return true;
  }
  bool page(Cursor &cursor) {
    cursor.values.clear(); cursor.pos = 0;
    for (unsigned attempts = 0; attempts < 1024; ++attempts) {
      std::string header;
      if (!read_at(cursor.offset,std::min<uint64_t>(65536,file_size_ - cursor.offset),header)) { return false; }
      CompactReader parser(header.data(),header.size()); CompactValue page_header;
      if (!parser.structure(page_header)) { return fail("invalid Parquet page header"); }
      int64_t compressed = page_header.field(3).number, uncompressed = page_header.field(2).number;
      if (compressed < 0 || uncompressed < 0 || uncompressed > static_cast<int64_t>(MAX_RECORD) || compressed > static_cast<int64_t>(MAX_RECORD)) { return fail("Parquet page exceeds limit"); }
      cursor.offset += parser.position();
      std::string payload, data;
      if (!read_at(cursor.offset,compressed,payload)) { return false; }
      cursor.offset += compressed;
      int type = page_header.field(1).number;
      bool v2 = type == 3;
      int64_t definition_bytes = v2 ? page_header.field(8).field(5).number : 0;
      int64_t repetition_bytes = v2 ? page_header.field(8).field(6).number : 0;
      if (definition_bytes < 0 || repetition_bytes < 0 || definition_bytes > static_cast<int64_t>(MAX_RECORD)
          || repetition_bytes > static_cast<int64_t>(MAX_RECORD)) { return fail("invalid Parquet level sizes"); }
      size_t levels = definition_bytes + repetition_bytes;
      if (levels > payload.size() || levels > static_cast<size_t>(uncompressed)) { return fail("invalid Parquet level lengths"); }
      bool compressed_data = cursor.codec != 0 && (!v2 || !page_header.field(8).has(7) || page_header.field(8).field(7).number);
      if (compressed_data) {
        std::string decoded;
        if (!snappy_decode(payload.substr(levels),uncompressed - levels,decoded)) { return fail("invalid Snappy page"); }
        data = payload.substr(0,levels) + decoded;
      } else { data = std::move(payload); if (data.size() != static_cast<size_t>(uncompressed)) { return fail("page length mismatch"); } }
      if (type == 2) {
        if (page_header.field(7).field(2).number != 0) { return fail("dictionary must use PLAIN encoding"); }
        size_t pos = 0;
        if (!plain(data,pos,page_header.field(7).field(1).number,cursor,cursor.dictionary)) { return false; }
        continue;
      }
      if (type != 0 && type != 3) { continue; }
      const auto &definition = page_header.field(v2 ? 8 : 5);
      int64_t count = definition.field(1).number;
      int encoding = definition.field(v2 ? 4 : 2).number;
      if (count < 0 || count > 262144) { return fail("invalid Parquet page value count"); }
      size_t pos = 0;
      std::vector<uint32_t> present;
      if (cursor.optional) {
        size_t length;
        if (v2) { length = definition.field(5).number; pos = definition.field(6).number; }
        else { if (data.size() < 4 || definition.field(3).number != 3) { return fail("unsupported definition-level encoding"); } length = little(data.data(),4); pos = 4; }
        if (!hybrid_decode(data,pos,length,1,count,present)) { return fail("invalid definition levels"); }
        pos += length;
      } else { present.assign(count,1); pos = levels; }
      size_t non_null = std::count(present.begin(),present.end(),1);
      std::vector<Cell> values;
      if (non_null == 0) {
      } else if (encoding == 0) {
        if (!plain(data,pos,non_null,cursor,values)) { return false; }
      } else if (encoding == 3 && cursor.physical == 0) {
        if (data.size() - pos < 4) { return fail("missing RLE boolean length"); }
        size_t length = little(data.data() + pos,4); pos += 4;
        std::vector<uint32_t> booleans;
        if (!hybrid_decode(data,pos,length,1,non_null,booleans)) { return fail("invalid RLE boolean values"); }
        for (uint32_t boolean : booleans) { values.push_back({boolean ? "true" : "false",false,false}); }
      } else if (encoding == 2 || encoding == 8) {
        if (pos >= data.size()) { return fail("missing dictionary bit width"); }
        unsigned width = static_cast<unsigned char>(data[pos++]); std::vector<uint32_t> indexes;
        if (!hybrid_decode(data,pos,data.size() - pos,width,non_null,indexes)) { return fail("invalid dictionary indexes"); }
        for (uint32_t index : indexes) {
          if (index >= cursor.dictionary.size()) { return fail("dictionary index out of range"); }
          values.push_back(cursor.dictionary[index]);
        }
      } else { return fail("unsupported Parquet data encoding"); }
      size_t index = 0;
      for (uint32_t level : present) { cursor.values.push_back(level ? values[index++] : Cell{"",true,false}); }
      size_t retained = 0;
      for (const auto &cached : cursors_) {
        retained += (cached.values.capacity() + cached.dictionary.capacity()) * sizeof(Cell);
        for (const auto &cell : cached.values) { retained += cell.text.capacity(); }
        for (const auto &cell : cached.dictionary) { retained += cell.text.capacity(); }
      }
      if (retained > 128 * 1024 * 1024) { return fail("Parquet projected page buffers exceed 128 MiB limit"); }
      if (!cursor.values.empty()) { return true; }
    }
    return fail("too many empty/index Parquet pages");
  }
  FILE *file_ = nullptr; uint64_t file_size_ = 0, group_ = 0, row_ = 0;
  CompactValue metadata_;
  std::vector<int> types_, converted_;
  std::vector<bool> optional_;
  std::vector<Cursor> cursors_;
  std::vector<bool> projection_;
};

class Reader {
public:
  Reader() = default;
  ~Reader() { if (file_) { std::fclose(file_); } }
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  bool open(const std::string &path, const std::string &secure, const Options &options) {
    options_ = options; secure_ = secure;
    if (options.kind == Kind::GLOB) { return open_glob(path); }
    if (!canonical(path, path_) || !allowed(path_, secure)) { return fail("path outside secure_file_priv or does not exist"); }
    if (!(file_ = std::fopen(path_.c_str(), "rb"))) { return fail("cannot open input"); }
    if (fstat(fileno(file_), &identity_) || !is_regular_mode(identity_.st_mode)) { return fail("input is not a regular file"); }
#ifndef _WIN32
    std::string actual;
    if (!canonical("/proc/self/fd/" + std::to_string(fileno(file_)), actual) || !allowed(actual, secure)) {
      return fail("input path changed while opening");
    }
#endif
    if (options.kind == Kind::PARQUET) {
      if (!parquet_.open(file_, columns)) { return fail(parquet_.error); }
      return true;
    }
    std::vector<Cell> row;
    if (options.kind == Kind::CSV) {
      int status = csv(row);
      if (status < 0) { return false; }
      if (status == 1) { return fail("empty CSV has no schema"); }
      if (row.size() > MAX_COLUMNS) { return fail("too many columns"); }
      for (size_t i = 0; i < row.size(); ++i) {
        std::string name = options.header ? row[i].text : "column" + std::to_string(i + 1);
        add_column(name, name);
      }
      if (!options.header) { infer_row(row); }
    }
    int status;
    while ((status = raw(row)) == 0) {
      if (!infer_row(row)) { return false; }
    }
    if (status < 0 || columns.empty()) { return fail(error.empty() ? "input has no columns" : error); }
    return rewind();
  }
  bool unchanged() {
    if (options_.kind == Kind::GLOB) { return true; }
    struct stat current{}, named{};
    if (!file_ || fstat(fileno(file_), &current) || stat(path_.c_str(), &named)
        || !same(identity_, current) || !same(identity_, named)) { return fail("input file changed after schema inference"); }
#ifndef _WIN32
    std::string actual;
    if (!canonical("/proc/self/fd/" + std::to_string(fileno(file_)),actual) || !allowed(actual,secure_)) {
      return fail("input moved outside secure_file_priv");
    }
#endif
    return true;
  }
  bool rewind() {
    if (options_.kind == Kind::GLOB) { glob_pos_ = 0; return true; }
    if (!unchanged()) { return false; }
    if (options_.kind == Kind::PARQUET) { parquet_.rewind(); return true; }
    std::rewind(file_); line_ = 1;
    if (options_.kind == Kind::CSV && options_.header) { std::vector<Cell> header; return csv(header) == 0; }
    return true;
  }
  int next_json(std::string &out) {
    if (!unchanged()) { return -1; }
    std::vector<Cell> row;
    if (options_.kind == Kind::GLOB) {
      if (glob_pos_ == files_.size()) { return 1; }
      std::string resolved;
      if (!canonical(files_[glob_pos_++], resolved) || !allowed(resolved, secure_)) { fail("discovered file escaped secure_file_priv"); return -1; }
      row.push_back({resolved, false, true});
    } else {
      int status = raw(row);
      if (status) { return status; }
    }
    out = "{";
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i) { out += ','; }
      out += json_string("c" + std::to_string(i)) + ':';
      if (i >= row.size() || row[i].null) { out += "null"; }
      else if (columns[i].type == Type::BOOLEAN || columns[i].type == Type::BIGINT || columns[i].type == Type::DOUBLE) {
        std::string text = lower(row[i].text);
        if (text == "true") { text = "1"; } else if (text == "false") { text = "0"; }
        // JSON does not accept a leading plus or redundant leading zeroes.
        if (columns[i].type == Type::DOUBLE) {
          char number[64]; std::snprintf(number, sizeof(number), "%.17g", std::strtod(text.c_str(), nullptr)); out += number;
        } else { out += std::to_string(std::strtoll(text.c_str(), nullptr, 10)); }
      } else {
        std::string text = row[i].text;
        if (columns[i].type == Type::DATETIME) {
          if (text.size() == 10) { text += " 00:00:00"; }
          else if (text.size() > 10 && text[10] == 'T') { text[10] = ' '; }
        }
        out += json_string(text);
      }
    }
    out += '}'; return 0;
  }
  void set_projection(const std::vector<bool> &projection) { parquet_.set_projection(projection); }
  std::string fingerprint() const {
    return std::to_string(identity_.st_dev) + ":" + std::to_string(identity_.st_ino) + ":"
      + std::to_string(identity_.st_size) + ":" + std::to_string(identity_.st_mtime)
#ifndef _WIN32
      + ":" + std::to_string(identity_.st_mtim.tv_nsec)
#endif
      ;
  }
  std::vector<Column> columns;
  std::string error;
private:
  bool fail(const std::string &message) { error = "file_sql " + path_ + ":" + std::to_string(line_) + ": " + message; return false; }
  static bool same(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && a.st_mtime == b.st_mtime
#ifndef _WIN32
      && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec
#endif
      ;
  }
  void add_column(std::string name, const std::string &key) {
    if (name.empty()) { name = "column" + std::to_string(columns.size() + 1); }
    std::string base = name; unsigned duplicate = 1;
    while (names_.count(lower(name))) { name = base + "__" + std::to_string(++duplicate); }
    names_.insert(lower(name)); columns.push_back({name, key, Type::NIL, rows_seen_ > 0});
  }
  bool infer_row(const std::vector<Cell> &row) {
    if (row.size() != columns.size()) { return fail("field count differs from schema"); }
    for (size_t i = 0; i < row.size(); ++i) {
      Type observed = infer(row[i]);
      columns[i].type = widen(columns[i].type, observed);
      columns[i].nullable = columns[i].nullable || row[i].null;
      if (observed == Type::DATETIME && row[i].text.size() > 19) {
        columns[i].datetime_scale = std::max(columns[i].datetime_scale,
            static_cast<unsigned>(row[i].text.size() - 20));
      }
    }
    ++rows_seen_;
    return true;
  }
  int csv(std::vector<Cell> &row) {
    row.clear(); Cell cell; bool inside = false, after = false, any = false; size_t bytes = 0;
    for (;;) {
      int c = std::fgetc(file_);
      if (c == EOF) {
        if (std::ferror(file_)) { fail("read failed"); return -1; }
        if (inside) { fail("unterminated quoted CSV field"); return -1; }
        if (!any && row.empty() && cell.text.empty()) { return 1; }
        cell.null = !cell.quoted && cell.text == options_.nullstr; row.push_back(cell); break;
      }
      any = true;
      if (++bytes > MAX_RECORD) { fail("record exceeds 16 MiB limit"); return -1; }
      if (inside) {
        if (c == options_.escape && options_.escape != options_.quote) {
          int escaped = std::fgetc(file_); if (escaped == EOF) { fail("incomplete CSV escape"); return -1; }
          cell.text += static_cast<char>(escaped);
        } else if (c == options_.quote) {
          int next = std::fgetc(file_);
          if (next == options_.quote && options_.escape == options_.quote) { cell.text += options_.quote; }
          else { inside = false; after = true; if (next != EOF) { std::ungetc(next, file_); } }
        } else { cell.text += static_cast<char>(c); if (c == '\n') { ++line_; } }
      } else if (c == options_.delimiter || c == '\n' || c == '\r') {
        cell.null = !cell.quoted && cell.text == options_.nullstr; row.push_back(cell); cell = Cell(); after = false;
        if (row.size() > MAX_COLUMNS) { fail("too many columns"); return -1; }
        if (c != options_.delimiter) {
          if (c == '\r') { int next = std::fgetc(file_); if (next != '\n' && next != EOF) { std::ungetc(next, file_); } }
          ++line_; break;
        }
      } else if (c == options_.quote && cell.text.empty() && !after) { inside = true; cell.quoted = true;
      } else if (after || c == options_.quote) { fail("unexpected character after CSV quote"); return -1;
      } else { cell.text += static_cast<char>(c); }
    }
    for (Cell &field : row) { if (!utf8(field.text)) { fail("invalid UTF-8"); return -1; } }
    if (line_ == 2 && !row.empty() && row[0].text.compare(0,3,"\xef\xbb\xbf") == 0) { row[0].text.erase(0,3); }
    return 0;
  }
  int raw(std::vector<Cell> &row) {
    if (options_.kind == Kind::PARQUET) {
      int status = parquet_.next(row); if (status < 0) { fail(parquet_.error); } return status;
    }
    if (options_.kind == Kind::CSV) { int status = csv(row); if (!status && row.size() != columns.size()) { fail("CSV field count mismatch"); return -1; } return status; }
    std::string line; int c;
    do {
      line.clear();
      while ((c = std::fgetc(file_)) != EOF && c != '\n') {
        line += static_cast<char>(c); if (line.size() > MAX_RECORD) { fail("JSONL record exceeds 16 MiB limit"); return -1; }
      }
      if (line.empty() && c == EOF) { return 1; }
      ++line_;
      if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    } while (line.find_first_not_of(" \t\r") == std::string::npos);
    if (!utf8(line)) { fail("invalid UTF-8"); return -1; }
    rapidjson::Document object;
    object.Parse(line.data(), line.size());
    if (object.HasParseError() || !object.IsObject()) { fail("JSONL line must be a flat object"); return -1; }
    std::map<std::string, Cell> values;
    for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member) {
      std::string key(member->name.GetString(), member->name.GetStringLength()); Cell value;
      if (values.count(key)) { fail("duplicate JSON key"); return -1; }
      const auto &v = member->value;
      if (v.IsNull()) { value.null = true; }
      else if (v.IsString()) { value.text.assign(v.GetString(),v.GetStringLength()); value.quoted = true; value.declared = Type::VARCHAR; }
      else if (v.IsBool()) { value.text = v.GetBool() ? "true" : "false"; value.declared = Type::BOOLEAN; }
      else if (v.IsInt64()) { value.text = std::to_string(v.GetInt64()); value.declared = Type::BIGINT; }
      else if (v.IsUint64()) { value.text = std::to_string(v.GetUint64()); value.declared = Type::VARCHAR; }
      else if (v.IsDouble() && std::isfinite(v.GetDouble())) { char n[64]; std::snprintf(n,sizeof(n),"%.17g",v.GetDouble()); value.text = n; value.declared = Type::DOUBLE; }
      else { fail("nested JSON objects/arrays or non-finite numbers are unsupported"); return -1; }
      values.emplace(key, value);
      bool found = false;
      for (const auto &column : columns) { if (column.key == key) { found = true; break; } }
      if (!found) { if (columns.size() >= MAX_COLUMNS) { fail("too many JSON columns"); return -1; } add_column(key, key); }
    }
    row.clear();
    for (const auto &column : columns) {
      auto value = values.find(column.key);
      row.push_back(value == values.end() ? Cell{"",true,false} : value->second);
    }
    return 0;
  }
  bool open_glob(const std::string &pattern) {
#ifndef _WIN32
    if (pattern.find('\0') != std::string::npos || pattern.find("://") != std::string::npos) { return fail("invalid glob path"); }
    size_t wildcard = pattern.find_first_of("*?[");
    size_t slash = pattern.rfind('/', wildcard == std::string::npos ? pattern.size() : wildcard);
    std::string directory, secure_root;
    canonical(secure_,secure_root);
    if (slash == std::string::npos || !canonical(pattern.substr(0, slash), directory)
        || (!allowed(directory, secure_) && directory != secure_root)) { return fail("glob directory outside secure_file_priv"); }
    glob_t matched{}; int code = ::glob(pattern.c_str(), GLOB_NOSORT, nullptr, &matched);
    if (code != 0 && code != GLOB_NOMATCH) { globfree(&matched); return fail("glob failed"); }
    for (size_t i = 0; i < matched.gl_pathc; ++i) {
      if (files_.size() >= 10000) { globfree(&matched); return fail("glob exceeds 10000 files"); }
      std::string path; struct stat st{};
      if (canonical(matched.gl_pathv[i],path) && allowed(path,secure_) && stat(path.c_str(),&st) == 0 && is_regular_mode(st.st_mode)) { files_.push_back(path); }
    }
    globfree(&matched); std::sort(files_.begin(),files_.end());
    columns.push_back({"file","file",Type::VARCHAR}); return true;
#else
    return fail("glob is currently supported on Linux");
#endif
  }
  FILE *file_ = nullptr;
  ParquetReader parquet_;
  Options options_;
  std::string path_, secure_;
  struct stat identity_{};
  uint64_t line_ = 1;
  uint64_t rows_seen_ = 0;
  std::set<std::string> names_;
  std::vector<std::string> files_;
  size_t glob_pos_ = 0;
};

static constexpr const char *COPY_MARKER = "__seekdb_copy_csv_v1__";
class CsvWriter {
public:
  ~CsvWriter() { abort(); }
  bool open(const std::string &config, const std::string &secure) {
#ifdef _WIN32
    error = "COPY currently requires a Linux file system"; return false;
#else
    rapidjson::Document d;
    if (config.compare(0,std::strlen(COPY_MARKER),COPY_MARKER) != 0) { return fail("invalid COPY configuration"); }
    d.Parse(config.c_str() + std::strlen(COPY_MARKER));
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("path") || !d["path"].IsString()
        || !d.HasMember("names") || !d["names"].IsArray()) { return fail("invalid COPY configuration"); }
    std::string path(d["path"].GetString(),d["path"].GetStringLength());
    if (path.empty() || path.find('\0') != std::string::npos || path.find("://") != std::string::npos) { return fail("invalid COPY path"); }
    size_t slash = path.find_last_of('/');
    std::string directory, basename = slash == std::string::npos ? path : path.substr(slash + 1);
    if (basename.empty() || basename == "." || basename == ".."
        || !canonical(slash == std::string::npos ? "." : path.substr(0,slash),directory)) { return fail("invalid COPY directory"); }
    target_ = directory + "/" + basename;
    if (!allowed(target_,secure)) { return fail("COPY target outside secure_file_priv"); }
    struct stat st{};
    if (lstat(target_.c_str(),&st) == 0 || errno != ENOENT) { return fail("COPY target already exists or cannot be accessed"); }
    if (d.HasMember("delimiter") && d["delimiter"].IsInt()) { delimiter_ = d["delimiter"].GetInt(); }
    if (d.HasMember("quote") && d["quote"].IsInt()) { quote_ = d["quote"].GetInt(); }
    if (d.HasMember("escape") && d["escape"].IsInt()) { escape_ = d["escape"].GetInt(); }
    if (d.HasMember("nullstr") && d["nullstr"].IsString()) { nullstr_.assign(d["nullstr"].GetString(),d["nullstr"].GetStringLength()); }
    if (!delimiter_ || !quote_ || !escape_ || delimiter_ == quote_) { return fail("invalid COPY CSV options"); }
    // Keep the destination directory open, so a concurrent rename cannot redirect
    // the final publication outside the directory checked above.
    dir_ = ::open(directory.c_str(),O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_ < 0) { return fail("cannot open COPY directory"); }
    secure_ = secure;
    std::string actual;
    if (!canonical("/proc/self/fd/" + std::to_string(dir_),actual)
        || !allowed(actual + "/" + basename,secure_)) { return fail("COPY directory changed during open"); }
    basename_ = basename;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
      temporary_ = ".seekdb-copy-" + std::to_string(getpid()) + "-" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "-" + std::to_string(attempt);
      int fd = openat(dir_,temporary_.c_str(),O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600);
      if (fd >= 0) { file_ = fdopen(fd,"wb"); if (!file_) { close(fd); } break; }
      if (errno != EEXIST) { break; }
    }
    if (!file_) { return fail("cannot create COPY temporary file"); }
    if (d.HasMember("header") && d["header"].IsBool() && d["header"].GetBool()) {
      std::vector<Cell> names;
      for (const auto &name : d["names"].GetArray()) {
        if (!name.IsString()) { return fail("invalid COPY header"); }
        names.push_back({std::string(name.GetString(),name.GetStringLength()),false,true});
      }
      if (!write(names)) { return false; }
    }
    return true;
#endif
  }
  bool write(const std::vector<Cell> &row) {
    std::string record;
    for (size_t i = 0; i < row.size(); ++i) {
      if (i) { record += delimiter_; }
      if (row[i].null) { record += nullstr_; }
      else {
        record += quote_;
        for (char c : row[i].text) { if (c == quote_ || (c == escape_ && escape_ != quote_)) { record += escape_; } record += c; }
        record += quote_;
      }
      if (record.size() > MAX_RECORD) { return fail("COPY row exceeds 16 MiB limit"); }
    }
    record += '\n';
    return std::fwrite(record.data(),1,record.size(),file_) == record.size() || fail("COPY write failed");
  }
  bool commit() {
#ifndef _WIN32
    std::string actual;
    if (!canonical("/proc/self/fd/" + std::to_string(dir_),actual)
        || !allowed(actual + "/" + basename_,secure_)) { return fail("COPY directory moved outside secure_file_priv"); }
    if (!file_ || std::fflush(file_) || fsync(fileno(file_))) { return fail("COPY flush failed"); }
    if (std::fclose(file_)) { file_ = nullptr; return fail("COPY close failed"); }
    file_ = nullptr;
    // linkat is atomic and refuses to replace an existing destination.
    if (linkat(dir_,temporary_.c_str(),dir_,basename_.c_str(),0)) { return fail("COPY publication failed; target may already exist"); }
    unlinkat(dir_,temporary_.c_str(),0); temporary_.clear(); close(dir_); dir_ = -1; return true;
#else
    return false;
#endif
  }
  void abort() {
    if (file_) { std::fclose(file_); file_ = nullptr; }
#ifndef _WIN32
    if (dir_ >= 0) { if (!temporary_.empty()) { unlinkat(dir_,temporary_.c_str(),0); } close(dir_); dir_ = -1; }
#endif
  }
  std::string error;
private:
  bool fail(const std::string &text) { error = text; return false; }
  FILE *file_ = nullptr;
  int dir_ = -1;
  char delimiter_ = ',', quote_ = '"', escape_ = '"';
  std::string nullstr_, target_, temporary_, basename_, secure_;
};
}}} // oceanbase::sql::file_sql
