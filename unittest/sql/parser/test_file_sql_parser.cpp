// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/parser/ob_parser.h"
#include "lib/allocator/ob_allocator.h"
#include <cstdio>
#include <iostream>
using namespace oceanbase::common;
using namespace oceanbase::sql;
static void dump(ParseNode *node, int level = 0) {
  if (!node || level > 15) { return; }
  std::printf("%*s type=%d value=%lld text=%.*s children=%d\n",level * 2,"",node->type_,
      static_cast<long long>(node->value_),node->str_value_ ? int(node->str_len_) : 0,
      node->str_value_ ? node->str_value_ : "",node->num_child_);
  for (int i = 0; i < node->num_child_; ++i) { dump(node->children_[i],level + 1); }
}
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; return 1; } } while (false)
int main(int argc,char **argv) {
  ObArenaAllocator allocator;
  ObParser parser(allocator,0);
  if (argc > 1) {
    ParseResult result;
    int ret = parser.parse(ObString::make_string(argv[1]),result);
    std::printf("ret=%d error=%s\n",ret,result.error_msg_);
    if (ret == 0) { dump(result.result_tree_); }
    return ret == 0 ? 0 : 1;
  }
  for (const char *sql : {
      "select * from read_csv('/tmp/test.csv')",
      "select * from read_json('/tmp/test.jsonl', format='newline_delimited') j",
      "select * from read_ndjson('/tmp/test.jsonl') as j",
      "select * from read_parquet('/tmp/test.parquet')",
      "select * from glob('/tmp/*.csv')",
      "select * from read_csv('/tmp/test.csv', header=true, sample_size=-1)"}) {
    ParseResult result;
    CHECK(parser.parse(ObString::make_string(sql),result) == 0);
    ParseNode *select = result.result_tree_->children_[0];
    CHECK(select->type_ == T_SELECT && select->value_ == 0);
    ParseNode *table = select->children_[PARSE_SELECT_FROM]->children_[0];
    CHECK(table->type_ == T_JSON_TABLE_EXPRESSION && table->value_ == 9001);
    CHECK(table->num_child_ == 3 && table->children_[1]->type_ == T_EXPR_LIST);
    CHECK(table->children_[1]->children_[0]->type_ == T_VARCHAR);
    CHECK(table->is_tree_not_param_);
  }
  for (const char *sql : {"describe select * from read_csv('/tmp/test.csv')",
                          "desc select * from read_parquet('/tmp/test.parquet')"}) {
    ParseResult result;
    CHECK(parser.parse(ObString::make_string(sql),result) == 0);
    ParseNode *explain = result.result_tree_->children_[0];
    CHECK(explain->type_ == T_EXPLAIN && explain->value_ == 0);
    CHECK(explain->children_[0] && explain->children_[0]->value_ == 9003);
  }
  for (const char *sql : {"copy test_table to '/tmp/result.csv' (format csv, header, nullstr 'NULL')",
                          "copy (select * from read_csv('/tmp/test.csv') order by id) to '/tmp/result.csv' (header)"}) {
    ParseResult result;
    CHECK(parser.parse(ObString::make_string(sql),result) == 0);
    ParseNode *select = result.result_tree_->children_[0];
    CHECK(select->type_ == T_SELECT && select->value_ == 0);
    ParseNode *into = select->children_[PARSE_SELECT_INTO_EXTRA];
    CHECK(into && into->type_ == T_INTO_OUTFILE && into->value_ == 9002 && into->is_tree_not_param_);
  }
  std::cout << "file_sql_parser: table functions, named options, DESCRIBE and COPY passed\n";
  return 0;
}
