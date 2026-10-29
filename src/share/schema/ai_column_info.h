/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "lib/hash/ob_hashmap.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObTableSchema;
bool has_ai_columns(const ObTableSchema &table);
// The durable definition travels with the column schema and its DDL transaction.
struct AIColumnInfo
{
  explicit AIColumnInfo(common::ObIAllocator &allocator) : allocator_(allocator) {}
  int from_json(const common::ObString &json);
  int to_json(common::ObString &json) const;
  int remap_ids(const common::hash::ObHashMap<uint64_t, uint64_t> &ids);

  common::ObString expression_;
  common::ObString normalized_expr_;
  common::ObString system_prompt_;
  common::ObSEArray<uint64_t, 8> dependencies_;
  uint64_t state_column_id_ = 0;
  uint64_t request_column_id_ = 0;
  uint64_t result_spec_column_id_ = 0;
  int64_t definition_version_ = 1;
  int64_t max_retries_ = 3;
  common::ObIAllocator &allocator_;
};

enum class AIColumnState : int64_t { PENDING = 0, READY = 1, FAILED = 2 };
}
}
}
