/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "share/schema/ob_table_schema.h"
#include "share/schema/ai_column_info.h"

namespace oceanbase
{
namespace sql
{
struct ObResolverParams;
class AIColumnDDL
{
public:
  static int resolve_attribute(common::ObIAllocator &allocator, const ParseNode &attribute,
                               share::schema::ObColumnSchemaV2 &column);
  static int finalize_table(ObResolverParams &params, share::schema::ObTableSchema &table,
                            uint64_t &last_column_id);
  static int finalize_column(ObResolverParams &params, share::schema::ObTableSchema &table,
                             share::schema::ObColumnSchemaV2 &column, uint64_t &last_column_id,
                             common::ObIArray<share::schema::ObColumnSchemaV2> &hidden,
                             const share::schema::AIColumnInfo *previous = nullptr);
};
}
}
