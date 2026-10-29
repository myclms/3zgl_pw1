/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/string/ob_string.h"
namespace oceanbase {
namespace share { namespace schema { class ObTableSchema; } }
namespace sql {
class ObExecContext;
class ObPhysicalPlan;
class ObDMLStmt;
class AIColumnSQL
{
public:
  static int collect_references(const ObDMLStmt &statement, ObPhysicalPlan &plan);
  static int check_read(ObExecContext &context, const ObPhysicalPlan &plan);
  static int retry(ObExecContext &context, const share::schema::ObTableSchema &table,
                    uint64_t column_id, const common::ObString &predicate);
};
} }
