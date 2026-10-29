/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/resolver/ddl/ai_column_ddl.h"
#include "share/schema/ai_column_info.h"
#include "sql/parser/parse_node.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;

int AIColumnDDL::resolve_attribute(ObIAllocator &allocator, const ParseNode &attribute,
                                  ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  AIColumnInfo info(allocator);
  if (column.is_ai_column() || attribute.num_child_ != 2 || attribute.children_[0] == nullptr
      || column.is_autoincrement() || column.is_rowkey_column()) {
    ret = OB_NOT_SUPPORTED;
  } else {
    info.expression_.assign_ptr(attribute.children_[0]->str_value_, attribute.children_[0]->str_len_);
    bool seen_sync = false, seen_retries = false;
    const ParseNode *options = attribute.children_[1];
    for (int64_t i = 0; OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
      const ParseNode *option = options->children_[i];
      if (option == nullptr || option->num_child_ != 2 || option->children_[0] == nullptr
          || option->children_[1] == nullptr) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const ParseNode *value = option->children_[1];
        ObString name(option->children_[0]->str_len_, option->children_[0]->str_value_);
        if (name.case_compare("sync_mode") == 0) {
          if (seen_sync || value->type_ != T_VARCHAR
              || ObString(value->str_len_, value->str_value_).case_compare("async") != 0) {
            ret = OB_NOT_SUPPORTED;
          }
          seen_sync = true;
        } else if (name.case_compare("max_retries") == 0) {
          if (seen_retries || value->type_ != T_INT || value->value_ < 0 || value->value_ > 100) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            info.max_retries_ = value->value_;
          }
          seen_retries = true;
        } else {
          ret = OB_NOT_SUPPORTED;
        }
      }
    }
    ObString json;
    if (OB_SUCC(ret) && OB_SUCC(ret = info.to_json(json))) {
      ret = column.set_ai_column_info(json);
    }
  }
  return ret;
}

static int print_ai_argument(ObResolverParams &params, ObRawExpr *expr, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const int64_t capacity = OB_MAX_DEFAULT_VALUE_LENGTH;
  char *buffer = static_cast<char *>(params.allocator_->alloc(capacity));
  int64_t position = 0;
  if (buffer == nullptr) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ObRawExprPrinter printer(buffer, capacity, &position, params.schema_checker_->get_schema_guard(),
                             params.session_info_->get_timezone_info());
    if (OB_SUCC(ret = printer.do_print(expr, T_NONE_SCOPE, true))) {
      ret = sql.append(buffer, position);
    }
  }
  return ret;
}

static int parse_ai_expression(ObResolverParams &params, const ObString &text, const ParseNode *&node)
{
  int ret = OB_SUCCESS;
  ObSqlString query;
  ObString stable;
  ParseResult parsed;
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode(),
                  params.session_info_->get_charsets4parser());
  if (OB_FAIL(query.append_fmt("SELECT %.*s FROM DUAL", text.length(), text.ptr()))) {
  } else if (OB_FAIL(ob_write_string(*params.allocator_, query.string(), stable))) {
  } else if (OB_FAIL(parser.parse(stable, parsed))) {
  } else if (parsed.result_tree_ == nullptr || parsed.result_tree_->num_child_ != 1
             || parsed.result_tree_->children_[0] == nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const ParseNode *statement = parsed.result_tree_->children_[0];
    const ParseNode *items = statement->type_ == T_SELECT ? statement->children_[PARSE_SELECT_SELECT] : nullptr;
    if (items == nullptr || items->num_child_ != 1 || items->children_[0] == nullptr
        || items->children_[0]->type_ != T_PROJECT_STRING || items->children_[0]->num_child_ < 1) {
      ret = OB_INVALID_ARGUMENT;
    } else { node = items->children_[0]->children_[0]; }
  }
  return ret;
}

int AIColumnDDL::finalize_column(ObResolverParams &params, ObTableSchema &table,
                                ObColumnSchemaV2 &column, uint64_t &last_column_id,
                                ObIArray<ObColumnSchemaV2> &hidden, const AIColumnInfo *previous)
{
  int ret = OB_SUCCESS;
  AIColumnInfo info(*params.allocator_);
  ObRawExpr *expr = nullptr;
  const ParseNode *node = nullptr;
  ObSEArray<ObQualifiedName, 8> names;
  if (!column.is_nullable() || column.is_rowkey_column() || column.is_autoincrement()) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(info.from_json(column.get_ai_column_info_str()))) {
  } else if (info.state_column_id_ != 0) {
    // Already resolved, for example an unchanged definition in ALTER TABLE.
  } else if (OB_FAIL(parse_ai_expression(params, info.expression_, node))) {
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(*params.expr_factory_,
                   *params.session_info_, *node, expr, names, &table, nullptr,
                   params.schema_checker_, ObResolverUtils::DISABLE_CHECK))) {
  } else if (expr == nullptr || (expr->get_expr_type() != T_FUN_SYS_AI_COMPLETE
                                && expr->get_expr_type() != T_FUN_SYS_AI_EMBED)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
      const ObQualifiedName &name = names.at(i);
      const ObColumnSchemaV2 *dependency = table.get_column_schema(name.col_name_);
      if (name.is_sys_func() || !name.database_name_.empty() || !name.tbl_name_.empty()
          || dependency == nullptr || dependency->is_hidden()) {
        ret = OB_ERR_BAD_FIELD_ERROR;
        LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, name.col_name_.length(), name.col_name_.ptr(),
                       9, "AI COLUMN");
      } else if (dependency->is_ai_column() || dependency->is_generated_column()
                 || dependency->get_column_id() == column.get_column_id()) {
        ret = OB_NOT_SUPPORTED;
      } else if (OB_FAIL(ObRawExprUtils::init_column_expr(*dependency,
                                                        params.session_info_, *name.ref_expr_))) {
      } else if (!has_exist_in_array(info.dependencies_, dependency->get_column_id())) {
        ret = info.dependencies_.push_back(dependency->get_column_id());
      }
    }
    if (OB_SUCC(ret)) {
      ret = expr->formalize(params.session_info_);
    }
    if (OB_SUCC(ret) && (expr->has_flag(CNT_SUB_QUERY) || expr->has_flag(CNT_AGG)
        || expr->has_flag(CNT_WINDOW_FUNC) || expr->has_flag(CNT_PL_UDF)
        || expr->has_flag(CNT_DYNAMIC_USER_VARIABLE) || expr->has_flag(CNT_STATE_FUNC))) {
      ret = OB_NOT_SUPPORTED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
      ret = ObRawExprUtils::check_is_valid_generated_col(expr->get_param_expr(i), *params.allocator_);
    }
    const ObRawExpr *model_expression = nullptr;
    if (OB_SUCC(ret) && (expr->get_param_count() < 2 || expr->get_param_count() > 3)) {
      ret = OB_INVALID_ARGUMENT;
    }
    if (OB_SUCC(ret)) {
      model_expression = expr->get_param_expr(0);
      while (model_expression != nullptr && model_expression->get_expr_type() == T_FUN_SYS_CAST) {
        model_expression = model_expression->get_param_expr(0);
      }
      if (model_expression == nullptr || model_expression->get_expr_class() != ObRawExpr::EXPR_CONST) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    if (OB_SUCC(ret)) {
      const ObObj &model = static_cast<const ObConstRawExpr *>(model_expression)->get_value();
      ObAIFuncExprInfo *model_info = nullptr;
      const bool embedding = expr->get_expr_type() == T_FUN_SYS_AI_EMBED;
      if (!model.is_string_type() || model.get_string().empty()) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*params.allocator_, model.get_string(), model_info))) {
      } else if (embedding ? !ObAIFuncUtils::is_dense_embedding_type(model_info)
                           : !ObAIFuncUtils::is_completion_type(model_info)) {
        ret = OB_INVALID_ARGUMENT;
      } else if (embedding != column.is_collection()) {
        ret = OB_NOT_SUPPORTED;
      } else if (embedding) {
        const ObIArray<ObString> &types = column.get_extended_type_info();
        int64_t dimension = 0;
        if (types.count() != 1) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          const ObString &type = types.at(0);
          const char *open = static_cast<const char *>(MEMCHR(type.ptr(), '(', type.length()));
          if (open == nullptr || type.length() < 8
              || ObString(6, type.ptr()).case_compare("VECTOR") != 0) {
            ret = OB_NOT_SUPPORTED;
          } else {
            for (const char *p = open + 1; p < type.ptr() + type.length() && *p != ')'; ++p) {
              if (*p < '0' || *p > '9' || dimension > 1000000) { ret = OB_INVALID_ARGUMENT; break; }
              dimension = dimension * 10 + (*p - '0');
            }
          }
        }
        if (OB_SUCC(ret) && expr->get_param_count() == 3) {
          ObRawExpr *dim = expr->get_param_expr(2);
          if (!dim->is_const_expr() || !static_cast<ObConstRawExpr *>(dim)->get_value().is_integer_type()) {
            ret = OB_INVALID_ARGUMENT;
          } else if (static_cast<ObConstRawExpr *>(dim)->get_value().get_int() != dimension) {
            ret = OB_ERR_INVALID_VECTOR_DIM;
            LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, dimension,
                           static_cast<ObConstRawExpr *>(dim)->get_value().get_int());
          }
        }
      } else if (column.is_json()) {
        info.system_prompt_ = ObString("Return exactly one valid JSON value, without Markdown or explanation.");
      } else if (ob_is_integer_type(column.get_data_type())) {
        info.system_prompt_ = ObString("Return exactly one base-10 integer, without Markdown or explanation.");
      } else if (ob_is_numeric_type(column.get_data_type())) {
        info.system_prompt_ = ObString("Return exactly one finite base-10 number, without Markdown or explanation.");
      } else if (!column.is_string_type() && !is_lob_storage(column.get_data_type())) {
        ret = OB_NOT_SUPPORTED;
      }
    }
    ObSqlString normalized;
    if (OB_SUCC(ret) && !info.system_prompt_.empty()) {
      if (OB_FAIL(normalized.append("AI_COMPLETE("))) {
      } else if (OB_FAIL(print_ai_argument(params, expr->get_param_expr(0), normalized))) {
      } else if (OB_FAIL(normalized.append(","))) {
      } else if (OB_FAIL(print_ai_argument(params, expr->get_param_expr(1), normalized))) {
      } else if (OB_FAIL(normalized.append(",JSON_MERGE_PATCH("))) {
      } else if (expr->get_param_count() == 3) {
        if (OB_SUCC(ret = normalized.append("COALESCE("))
            && OB_SUCC(ret = print_ai_argument(params, expr->get_param_expr(2), normalized))) {
          ret = normalized.append(",JSON_OBJECT())");
        }
      } else {
        ret = normalized.append("JSON_OBJECT()");
      }
      if (OB_SUCC(ret)) {
        ret = normalized.append_fmt(",JSON_OBJECT('system_prompt','%.*s')))",
                                     info.system_prompt_.length(), info.system_prompt_.ptr());
      }
      info.normalized_expr_ = normalized.string();
    } else if (OB_SUCC(ret)) {
      // The worker uses its own SQL mode. Persist parsed literals and identifiers
      // rather than reinterpreting the user's source under that different mode.
      if (OB_SUCC(ret = print_ai_argument(params, expr, normalized))) {
        info.normalized_expr_ = normalized.string();
      }
    }
    if (OB_SUCC(ret) && previous != nullptr) {
      info.state_column_id_ = previous->state_column_id_;
      info.request_column_id_ = previous->request_column_id_;
      info.result_spec_column_id_ = previous->result_spec_column_id_;
      info.definition_version_ = previous->definition_version_ + 1;
    }
    for (int64_t i = 0; OB_SUCC(ret) && previous == nullptr && i < 3; ++i) {
      ObColumnSchemaV2 field(params.allocator_);
      ObSqlString name;
      const char *kind = i == 0 ? "state" : i == 1 ? "request" : "spec";
      if (OB_FAIL(name.append_fmt("__ai_%s_%lu", kind, column.get_column_id()))) {
      } else if (table.get_column_schema(name.string()) != nullptr) {
        ret = OB_ERR_COLUMN_DUPLICATE;
      } else if (OB_FAIL(field.set_column_name(name.string()))) {
      } else {
        field.set_column_id(++last_column_id);
        field.set_table_id(table.get_table_id());
        field.set_is_hidden(true);
        field.set_nullable(true);
        field.set_data_type(i == 1 ? ObVarcharType : ObIntType);
        if (i == 1) {
          field.set_data_length(64);
          field.set_charset_type(CHARSET_UTF8MB4);
          field.set_collation_type(CS_TYPE_UTF8MB4_BIN);
          info.request_column_id_ = field.get_column_id();
        } else if (i == 0) {
          info.state_column_id_ = field.get_column_id();
        } else {
          info.result_spec_column_id_ = field.get_column_id();
        }
        ret = hidden.push_back(field);
      }
    }
    ObString json;
    if (OB_SUCC(ret) && OB_SUCC(ret = info.to_json(json))) {
      ret = column.set_ai_column_info(json);
    }
  }
  return ret;
}

int AIColumnDDL::finalize_table(ObResolverParams &params, ObTableSchema &table, uint64_t &last_column_id)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObColumnSchemaV2, 8> hidden;
  const int64_t count = table.get_column_count();
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    ObColumnSchemaV2 *column = table.get_column_schema_by_idx(i);
    if (column->is_ai_column()) {
      ret = finalize_column(params, table, *column, last_column_id, hidden);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < hidden.count(); ++i) {
    ret = table.add_column(hidden.at(i));
  }
  if (OB_SUCC(ret)) { table.set_max_used_column_id(last_column_id); }
  return ret;
}
}
}
