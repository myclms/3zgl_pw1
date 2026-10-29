/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/expr/ob_expr_ai/ai_column_evaluation.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "share/schema/ob_column_schema.h"

namespace oceanbase { namespace sql {
static thread_local const share::schema::ObColumnSchemaV2 *evaluation_column = nullptr;

AIColumnEvaluationScope::AIColumnEvaluationScope(const share::schema::ObColumnSchemaV2 &column)
  : previous_(evaluation_column) { evaluation_column = &column; }
AIColumnEvaluationScope::~AIColumnEvaluationScope() { evaluation_column = previous_; }
bool AIColumnEvaluationScope::active() { return evaluation_column != nullptr; }

int AIColumnEvaluationScope::convert_result(ObRawExprFactory &factory, ObSQLSessionInfo &session,
                                           ObRawExpr *&expression)
{
  int ret = common::OB_SUCCESS;
  if (evaluation_column != nullptr && expression != nullptr) {
    const ObItemType type = expression->get_expr_type();
    if (type == T_FUN_SYS_AI_COMPLETE || type == T_FUN_SYS_AI_EMBED) {
      const auto &column = *evaluation_column;
      ret = ObRawExprUtils::build_column_conv_expr(&session, factory, column.get_data_type(),
          column.get_collation_type(), column.get_accuracy().get_accuracy(), true, nullptr,
          &column.get_extended_type_info(), expression);
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < expression->get_param_count(); ++i) {
        ObRawExpr *child = expression->get_param_expr(i);
        if (OB_SUCC(ret = SMART_CALL(convert_result(factory, session, child)))) {
          expression->get_param_expr(i) = child;
        }
      }
    }
  }
  return ret;
}
} }
