// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/engine/basic/file_sql_reader.h"
#include <fstream>
#include <iostream>

using namespace oceanbase::sql::file_sql;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; return 1; } } while (false)
static bool put(const std::string &path, const std::string &data) {
  std::ofstream stream(path,std::ios::binary); stream << data; return bool(stream);
}
int main(int argc, char **argv) {
  CHECK(argc == 3);
  std::string root = argv[1], fixtures = argv[2];
  std::string path = root + "/sample.csv";
  CHECK(put(path,"id,name,score,active,day,created,note,id\r\n1,Alice,98.5,true,2026-08-01,2026-08-01 10:11:12,\"hello,\r\nworld\",101\r\n2,Bob,87,false,,,plain,102\r\n3,Chen,90.25,true,2026-08-03,2026-08-03T12:13:14,\"say \"\"hi\"\"\",103\r\n"));
  Options csv; Reader reader;
  CHECK(reader.open(path,root,csv));
  CHECK(reader.columns.size() == 8 && reader.columns[7].name == "id__2");
  CHECK(reader.columns[0].type == Type::BIGINT && reader.columns[2].type == Type::DOUBLE);
  CHECK(reader.columns[3].type == Type::BOOLEAN && reader.columns[4].type == Type::DATE && reader.columns[5].type == Type::DATETIME);
  std::string row;
  CHECK(reader.next_json(row) == 0 && row.find("hello,\\u000d\\u000aworld") != std::string::npos);
  CHECK(reader.next_json(row) == 0 && row.find("\"c4\":null") != std::string::npos);
  CHECK(reader.next_json(row) == 0 && row.find("say \\\"hi\\\"") != std::string::npos);
  CHECK(reader.next_json(row) == 1);
  CHECK(reader.rewind() && reader.next_json(row) == 0);
  std::string config = encode_config(path,csv,reader.fingerprint(),reader.columns);
  std::string decoded, identity, schema; Options options;
  CHECK(decode_config(config,decoded,options,identity,schema) && decoded == path && schema == schema_signature(reader.columns));
  CHECK(put(path,"id\n42\n")); CHECK(reader.next_json(row) == -1);
  Reader escape; CHECK(!escape.open("/etc/passwd",root,csv));
  CHECK(put(root + "/bad.csv","a,b\n1\n")); Reader bad;
  CHECK(!bad.open(root + "/bad.csv",root,csv) && bad.error.find("field count") != std::string::npos);
  CHECK(put(root + "/quoted.csv","empty,nul,large,unicode\n\"\",,9223372036854775808,中文\n")); Reader quoted;
  CHECK(quoted.open(root + "/quoted.csv",root,csv));
  CHECK(quoted.columns[2].type == Type::VARCHAR);
  CHECK(quoted.next_json(row) == 0 && row.find("\"c0\":\"\"") != std::string::npos && row.find("\"c1\":null") != std::string::npos);
  CHECK(put(root + "/bad_utf8.csv",std::string("a\n") + char(0xC0) + char(0x80)));
  Reader utf; CHECK(!utf.open(root + "/bad_utf8.csv",root,csv));
  CHECK(infer({"2024-02-29",false,false}) == Type::DATE);
  CHECK(infer({"2023-02-29",false,false}) == Type::VARCHAR);
  CHECK(infer({"2024-01-01 24:00:00",false,false}) == Type::VARCHAR);
  CHECK(put(root + "/sample.jsonl","{\"id\":1,\"a\":true}\n{\"id\":2,\"later\":3.5}\n{\"a\":null,\"id\":3}\n"));
  Options json; json.kind = Kind::JSONL; Reader union_reader;
  CHECK(union_reader.open(root + "/sample.jsonl",root,json));
  CHECK(union_reader.columns.size() == 3 && union_reader.columns[2].type == Type::DOUBLE);
  CHECK(union_reader.next_json(row) == 0 && row.find("\"c2\":null") != std::string::npos);
  CHECK(union_reader.next_json(row) == 0 && row.find("\"c1\":null") != std::string::npos);
  CHECK(put(root + "/typed.jsonl","{\"string\":\"001\",\"real\":1.0}\n"));
  Reader typed;
  CHECK(typed.open(root + "/typed.jsonl",root,json));
  CHECK(typed.columns[0].type == Type::VARCHAR && typed.columns[1].type == Type::DOUBLE);
  CHECK(put(root + "/nested.jsonl","{\"a\":[1]}\n")); Reader nested;
  CHECK(!nested.open(root + "/nested.jsonl",root,json));
  Options glob; glob.kind = Kind::GLOB; Reader discovery;
  CHECK(discovery.open(root + "/*.csv",root,glob));
  unsigned discovered = 0; while (discovery.next_json(row) == 0) { ++discovered; }
  CHECK(discovered >= 3);
  Options parquet; parquet.kind = Kind::PARQUET; Reader columnar;
  CHECK(columnar.open(fixtures + "/simple.parquet",fixtures,parquet));
  unsigned records = 0; while (columnar.next_json(row) == 0) { ++records; }
  if (records != 7) { std::cerr << "Parquet records=" << records << " error=" << columnar.error << "\n"; }
  CHECK(records == 7);
  CHECK(columnar.rewind() && columnar.next_json(row) == 0);
  std::string target = root + "/export.csv";
  std::string copy = std::string(COPY_MARKER) + "{\"path\":" + json_string(target)
      + ",\"header\":true,\"nullstr\":\"NULL\",\"names\":[\"id\",\"text\"]}";
  {
    CsvWriter writer; CHECK(writer.open(copy,root));
    CHECK(writer.write({{"1",false,false},{"hello\n\"world\"",false,false}}));
    CHECK(writer.write({{"2",false,false},{"",true,false}}));
    CHECK(writer.commit());
  }
  CsvWriter overwrite; CHECK(!overwrite.open(copy,root));
  Options exported; exported.nullstr = "NULL"; Reader roundtrip;
  CHECK(roundtrip.open(target,root,exported) && roundtrip.next_json(row) == 0 && row.find("hello") != std::string::npos);
  CHECK(roundtrip.next_json(row) == 0 && row.find("\"c1\":null") != std::string::npos);
  {
    CsvWriter failed; std::string temporary = std::string(COPY_MARKER) + "{\"path\":" + json_string(root + "/aborted.csv") + ",\"names\":[]}";
    CHECK(failed.open(temporary,root)); CHECK(failed.write({{"partial",false,false}}));
  }
  struct stat st{}; CHECK(stat((root + "/aborted.csv").c_str(),&st) != 0);
  std::string decoded_snappy;
  CHECK(snappy_decode(std::string("\x05\x10hello",7),5,decoded_snappy) && decoded_snappy == "hello");
  CHECK(!snappy_decode(std::string("\x05\x01\x00",3),5,decoded_snappy));
  uint32_t seed = 0x12345678;
  for (unsigned trial = 0; trial < 2000; ++trial) {
    std::string bytes;
    for (unsigned i = 0; i < trial % 257; ++i) { seed = seed * 1664525U + 1013904223U; bytes += char(seed >> 24); }
    snappy_decode(bytes,trial % 1024,decoded_snappy);
    CompactValue value; CompactReader parser(bytes.data(),bytes.size()); parser.structure(value);
  }
  std::cout << "file_scan_utils: CSV, JSONL, Parquet, COPY, bounds and file mutation checks passed\n";
  return 0;
}
