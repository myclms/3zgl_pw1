/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/string/ob_sql_string.h"
#include "share/schema/ai_column_info.h"
#include "share/ob_define.h"

namespace oceanbase
{
namespace sql
{
class ObTableModifyOp;
class ObExecContext;
struct ObDMLBaseCtDef;

// Mark only SQL executed recursively for this parent statement. Transaction
// control must keep the outer statement open until its durable tasks are saved.
class AIColumnNestedSQLScope
{
public:
  explicit AIColumnNestedSQLScope(const ObExecContext &parent);
  ~AIColumnNestedSQLScope();
  static bool is_nested(ObExecContext *context);
private:
  const ObExecContext *previous_;
};

// This scope is held by kernel-generated RETRY/publication SQL only. Plans
// compiled in it carry a runtime guard so they cannot be reused by user DML.
class AIColumnWriteScope
{
public:
  explicit AIColumnWriteScope(uint64_t retry_column_id = 0);
  ~AIColumnWriteScope();
  static bool active();
  static uint64_t retry_column_id();
private:
  bool old_active_;
  uint64_t old_retry_column_id_;
};

class AIColumnDML
{
public:
  static int before_row(ObTableModifyOp &op, const ObDMLBaseCtDef &definition, bool inserting);
  static int after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &definition, common::ObDmlEventType event);
  static int append_identifier(common::ObSqlString &sql, const common::ObString &name);
  static int append_string(common::ObSqlString &sql, const common::ObString &value);
  static int append_column(common::ObSqlString &sql, const share::schema::ObTableSchema &table, uint64_t column_id);
};
}
}
