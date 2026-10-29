/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

namespace oceanbase
{
namespace share { namespace schema { class ObColumnSchemaV2; } }
namespace sql
{
class ObRawExpr;
class ObRawExprFactory;
class ObSQLSessionInfo;

// Only background evaluation enters this scope. The result is converted with
// the same column conversion expression used by ordinary DML, before it is
// recorded as a successful task. Transport retries belong to the durable task.
class AIColumnEvaluationScope
{
public:
  explicit AIColumnEvaluationScope(const share::schema::ObColumnSchemaV2 &column);
  ~AIColumnEvaluationScope();
  static bool active();
  static int convert_result(ObRawExprFactory &factory, ObSQLSessionInfo &session, ObRawExpr *&expression);
private:
  const share::schema::ObColumnSchemaV2 *previous_;
};
}
}
