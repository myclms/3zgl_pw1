/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/ob_sql_context.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include <openssl/rand.h>

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;

static thread_local bool ai_write_active = false;
static thread_local uint64_t ai_retry_target = 0;
static thread_local const ObExecContext *ai_nested_parent = nullptr;

AIColumnNestedSQLScope::AIColumnNestedSQLScope(const ObExecContext &parent)
  : previous_(ai_nested_parent) { ai_nested_parent = &parent; }
AIColumnNestedSQLScope::~AIColumnNestedSQLScope() { ai_nested_parent = previous_; }
bool AIColumnNestedSQLScope::is_nested(ObExecContext *context)
{
  return ai_nested_parent != nullptr && context != nullptr
      && context->get_parent_ctx() == ai_nested_parent;
}

AIColumnWriteScope::AIColumnWriteScope(uint64_t retry_column_id)
  : old_active_(ai_write_active), old_retry_column_id_(ai_retry_target)
{
  ai_write_active = true;
  ai_retry_target = retry_column_id;
}
AIColumnWriteScope::~AIColumnWriteScope()
{
  ai_write_active = old_active_;
  ai_retry_target = old_retry_column_id_;
}
bool AIColumnWriteScope::active() { return ai_write_active; }
uint64_t AIColumnWriteScope::retry_column_id() { return ai_retry_target; }

int AIColumnDML::append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = sql.append(name.ptr() + i, 1);
    if (OB_SUCC(ret) && name.ptr()[i] == '`') { ret = sql.append("`"); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int AIColumnDML::append_string(ObSqlString &sql, const ObString &value)
{
  // Hex literals are independent of NO_BACKSLASH_ESCAPES and cannot terminate
  // generated SQL, including for embedded NULs and quoted identifiers in keys.
  static const char hex[] = "0123456789abcdef";
  int ret = sql.append("X'");
  char buffer[512];
  for (int64_t offset = 0; OB_SUCC(ret) && offset < value.length(); offset += sizeof(buffer) / 2) {
    int64_t count = MIN(static_cast<int64_t>(sizeof(buffer) / 2), value.length() - offset);
    for (int64_t i = 0; i < count; ++i) {
      unsigned char byte = static_cast<unsigned char>(value.ptr()[offset + i]);
      buffer[2 * i] = hex[byte >> 4];
      buffer[2 * i + 1] = hex[byte & 15];
    }
    ret = sql.append(buffer, count * 2);
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}

int AIColumnDML::append_column(ObSqlString &sql, const ObTableSchema &table, uint64_t column_id)
{
  const ObColumnSchemaV2 *column = table.get_column_schema(column_id);
  return column == nullptr ? OB_SCHEMA_ERROR : append_identifier(sql, column->get_column_name_str());
}

static ObExpr *ai_row_expression(const ObDMLBaseCtDef &definition, uint64_t column_id, bool old_row)
{
  ObExpr *expression = nullptr;
  const ExprFixedArray &row = old_row ? definition.old_row_ : definition.new_row_;
  for (int64_t i = 0; expression == nullptr && i < definition.column_ids_.count() && i < row.count(); ++i) {
    if (definition.column_ids_.at(i) == column_id) { expression = row.at(i); }
  }
  return expression;
}

static int ai_table_schema(const ObDMLBaseCtDef &definition,
                           ObSchemaGetterGuard &guard, const ObTableSchema *&table)
{
  int ret = OB_SUCCESS;
  table = nullptr;
  if (GCTX.schema_service_ == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(definition.das_base_ctdef_.index_tid_, table))) {
  } else if (table == nullptr) {
    ret = OB_TABLE_NOT_EXIST;
  }
  return ret;
}

static int ai_column_changed(ObTableModifyOp &op, const ObDMLBaseCtDef &definition,
                              uint64_t column_id, bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  ObExpr *old_expression = ai_row_expression(definition, column_id, true);
  ObExpr *new_expression = ai_row_expression(definition, column_id, false);
  ObDatum *old_value = nullptr, *new_value = nullptr;
  if (old_expression == nullptr || new_expression == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(old_expression->eval(op.get_eval_ctx(), old_value))) {
  } else if (OB_FAIL(new_expression->eval(op.get_eval_ctx(), new_value))) {
  } else if (old_value->is_null() || new_value->is_null()) {
    changed = old_value->is_null() != new_value->is_null();
  } else if (is_lob_storage(old_expression->datum_meta_.type_)) {
    int64_t difference = 0;
    if (OB_SUCC(ret = ObDMLService::check_lob_column_changed(op.get_eval_ctx(), *old_expression,
                                *old_value, *new_expression, *new_value, difference))) {
      changed = difference != 0;
    }
  } else {
    // Compare the actual input, not a case/accent-insensitive SQL collation.
    changed = old_value->len_ != new_value->len_
           || (old_value->len_ != 0 && MEMCMP(old_value->ptr_, new_value->ptr_, old_value->len_) != 0);
  }
  return ret;
}

static int ai_new_request_id(ObIAllocator &allocator, ObString &request)
{
  unsigned char bytes[16];
  char text[32];
  const char *hex = "0123456789abcdef";
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) { return OB_ERR_UNEXPECTED; }
  for (int64_t i = 0; i < sizeof(bytes); ++i) {
    text[2 * i] = hex[bytes[i] >> 4];
    text[2 * i + 1] = hex[bytes[i] & 15];
  }
  return ob_write_string(allocator, ObString(sizeof(text), text), request);
}

int AIColumnDML::before_row(ObTableModifyOp &op, const ObDMLBaseCtDef &definition, bool inserting)
{
  int ret = OB_SUCCESS;
  if (!definition.has_ai_columns_) { return ret; }
  if (definition.ai_internal_write_ && !AIColumnWriteScope::active()) { return OB_NOT_SUPPORTED; }
  const ObTableSchema *table = nullptr;
  bool primary_key_changed = false;
  ObString request;
  ObArenaAllocator temporary("AIColumnDML");
  ObSchemaGetterGuard guard;
  if (OB_FAIL(ai_table_schema(definition, guard, table))) {
    LOG_WARN("failed to read AI column DML schema", K(ret), K(definition.das_base_ctdef_.index_tid_));
    return ret;
  }
  for (int64_t i = 0; OB_SUCC(ret) && !inserting && !primary_key_changed
                       && i < table->get_rowkey_column_num(); ++i) {
    ret = ai_column_changed(op, definition, table->get_rowkey_info().get_column(i)->column_id_,
                            primary_key_changed);
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 &column = *table->get_column_schema_by_idx(i);
    if (!column.is_ai_column()) { continue; }
    AIColumnInfo info(temporary);
    bool regenerate = inserting || primary_key_changed;
    if (OB_FAIL(info.from_json(column.get_ai_column_info_str()))) {
    } else if (AIColumnWriteScope::retry_column_id() != 0) {
      regenerate = AIColumnWriteScope::retry_column_id() == column.get_column_id();
    } else {
      for (int64_t j = 0; OB_SUCC(ret) && !regenerate && j < info.dependencies_.count(); ++j) {
        ret = ai_column_changed(op, definition, info.dependencies_.at(j), regenerate);
      }
    }
    if (OB_SUCC(ret) && regenerate) {
      if (request.empty()) { ret = ai_new_request_id(op.get_exec_ctx().get_allocator(), request); }
      const uint64_t ids[] = {column.get_column_id(), info.state_column_id_,
                              info.request_column_id_, info.result_spec_column_id_};
      for (int64_t j = 0; OB_SUCC(ret) && j < 4; ++j) {
        ObExpr *expression = ai_row_expression(definition, ids[j], false);
        if (expression == nullptr) {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("AI column missing from DML new row", K(ret), K(ids[j]), K(definition.column_ids_), K(definition.new_row_.count()));
        } else {
          op.clear_dml_evaluated_flag(expression);
          ObDatum &datum = expression->locate_datum_for_write(op.get_eval_ctx());
          if (j == 0) { datum.set_null(); }
          else if (j == 1) { datum.set_int(static_cast<int64_t>(AIColumnState::PENDING)); }
          else if (j == 2) { datum.set_string(request); }
          else { datum.set_int(info.definition_version_); }
          expression->set_evaluated_flag(op.get_eval_ctx());
        }
      }
    }
  }
  return ret;
}

static int ai_row_predicate(ObTableModifyOp &op, const ObDMLBaseCtDef &definition,
                            const ObTableSchema &table, bool old_row, ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_rowkey_column_num(); ++i) {
    const uint64_t id = table.get_rowkey_info().get_column(i)->column_id_;
    const ObColumnSchemaV2 *column = table.get_column_schema(id);
    ObExpr *expression = ai_row_expression(definition, id, old_row);
    ObDatum *datum = nullptr;
    ObObj value;
    if (column == nullptr || expression == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expression->eval(op.get_eval_ctx(), datum))) {
    } else if (datum->is_null()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(datum->to_obj(value, expression->obj_meta_, expression->obj_datum_map_))) {
    } else if (i != 0 && OB_FAIL(predicate.append(" AND "))) {
    } else if (OB_FAIL(AIColumnDML::append_identifier(predicate, column->get_column_name_str()))) {
    } else if (OB_FAIL(predicate.append(" = "))) {
    } else if (value.is_string_type()) {
      if (value.get_collation_type() == CS_TYPE_BINARY) {
        ret = AIColumnDML::append_string(predicate, value.get_string());
      } else if (OB_SUCC(ret = predicate.append("CONVERT("))
                 && OB_SUCC(ret = AIColumnDML::append_string(predicate, value.get_string()))) {
        ret = predicate.append_fmt(" USING %s)", ObCharset::charset_name(column->get_charset_type()));
      }
    } else {
      char *buffer = nullptr;
      int64_t capacity = 0, position = 0;
      if (OB_SUCC(ret = value.print_sql_literal(buffer, capacity, position, op.get_exec_ctx().get_allocator()))) {
        ret = predicate.append(buffer, position);
      }
    }
  }
  return ret;
}

int AIColumnDML::after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &definition, ObDmlEventType event)
{
  int ret = OB_SUCCESS;
  if (!definition.has_ai_columns_) { return ret; }
  if (definition.ai_internal_write_ && !AIColumnWriteScope::active()) { return OB_NOT_SUPPORTED; }
  const bool deleting = event == DE_DELETING;
  const bool inserting = event == DE_INSERTING;
  const ObTableSchema *table = nullptr;
  ObArenaAllocator temporary("AIColumnTask");
  ObSEArray<uint64_t, 4> columns;
  ObSEArray<int64_t, 4> versions;
  ObSEArray<ObString, 4> requests;
  ObSqlString old_predicate, new_predicate, cleanup, insert;
  ObSchemaGetterGuard guard;
  if (OB_FAIL(ai_table_schema(definition, guard, table))) {
    LOG_WARN("failed to read AI column task schema", K(ret));
    return ret;
  }
  for (int64_t i = 0; OB_SUCC(ret) && !deleting && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 &column = *table->get_column_schema_by_idx(i);
    if (!column.is_ai_column()) { continue; }
    AIColumnInfo info(temporary);
    bool changed = inserting;
    if (OB_FAIL(info.from_json(column.get_ai_column_info_str()))) {
    } else if (!inserting && OB_FAIL(ai_column_changed(op, definition, info.request_column_id_, changed))) {
    } else if (changed) {
      ObExpr *expression = ai_row_expression(definition, info.request_column_id_, false);
      ObDatum *datum = nullptr;
      if (expression == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(expression->eval(op.get_eval_ctx(), datum))) {
      } else if (datum->is_null()) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(columns.push_back(column.get_column_id()))) {
      } else if (OB_FAIL(versions.push_back(info.definition_version_))) {
      } else { ret = requests.push_back(datum->get_string()); }
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("AI column task could not read the new request", K(ret)); return ret;
  }
  if (!deleting && columns.empty()) { return ret; }
  if (!inserting) { ret = ai_row_predicate(op, definition, *table, true, old_predicate); }
  if (OB_SUCC(ret) && !deleting) { ret = ai_row_predicate(op, definition, *table, false, new_predicate); }
  if (OB_SUCC(ret)) {
    ret = cleanup.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_predicate=",
                              table->get_table_id());
  }
  if (OB_SUCC(ret)) { ret = append_string(cleanup, inserting ? new_predicate.string() : old_predicate.string()); }
  if (OB_SUCC(ret) && !deleting) {
    if (OB_SUCC(ret = cleanup.append(" AND column_id IN ("))) {
      for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
        ret = cleanup.append_fmt(i == 0 ? "%lu" : ",%lu", columns.at(i));
      }
      if (OB_SUCC(ret)) { ret = cleanup.append(")"); }
    }
    if (OB_SUCC(ret)) {
      ret = insert.append("INSERT INTO oceanbase.__all_ai_column_task "
          "(table_id,request_id,column_id,rowkey_predicate,definition_version,state,retry_count,eligible_ts,owner,last_error) VALUES ");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      if (OB_FAIL(insert.append_fmt(i == 0 ? "(%lu," : ",(%lu,", table->get_table_id()))) {
      } else if (OB_FAIL(append_string(insert, requests.at(i)))) {
      } else if (OB_FAIL(insert.append_fmt(",%lu,", columns.at(i)))) {
      } else if (OB_FAIL(append_string(insert, new_predicate.string()))) {
      } else { ret = insert.append_fmt(",%ld,0,0,NOW(6),'',0)", versions.at(i)); }
    }
  }
  if (OB_SUCC(ret) && OB_SUCC(ret = op.open_inner_conn())) {
    AIColumnNestedSQLScope scope(op.get_exec_ctx());
    if (OB_SUCC(ret = op.begin_nested_session(false))) {
      if (OB_SUCC(ret = op.execute_write(cleanup.ptr())) && !deleting) {
        ret = op.execute_write(insert.ptr());
      }
      int restore_ret = op.end_nested_session();
      if (OB_SUCC(ret)) { ret = restore_ret; }
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("AI column task transaction failed", K(ret), K(event));
  }
  return ret;
}
}
}
