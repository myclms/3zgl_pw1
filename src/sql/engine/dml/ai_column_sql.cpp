/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_sql.h"
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_del_upd_stmt.h"
#include "sql/resolver/dml/ob_stmt_expr_visitor.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/api/query/session/ob_inner_sql_connection_access.h"
#include "sql/ob_sql_context.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;

static int collect_ai_references(const ObDMLStmt &statement, ObPhysicalPlan &plan,
                                 const ObIArray<int64_t> *projected = nullptr)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr *, 16> expressions, references;
  ObStmtExprGetter visitor;
  visitor.set_relation_scope();
  visitor.remove_scope(SCOPE_DML_COLUMN);
  visitor.remove_scope(SCOPE_DML_VALUE);
  visitor.remove_scope(SCOPE_DMLINFOS);
  visitor.remove_scope(SCOPE_DML_CONSTRAINT);
  const ObSelectStmt *select = statement.is_select_stmt() ? static_cast<const ObSelectStmt *>(&statement) : nullptr;
  const bool prune_projection = projected != nullptr && select != nullptr
                             && !select->is_distinct() && !select->is_set_stmt();
  if (prune_projection) { visitor.remove_scope(SCOPE_SELECT); }
  if (OB_FAIL(statement.get_relation_exprs(expressions, visitor))) { return ret; }
  for (int64_t i = 0; OB_SUCC(ret) && prune_projection && i < projected->count(); ++i) {
    const int64_t index = projected->at(i);
    if (index < 0 || index >= select->get_select_item_size()) { ret = OB_ERR_UNEXPECTED; }
    else { ret = expressions.push_back(select->get_select_item(index).expr_); }
  }
  if (statement.is_dml_write_stmt()) {
    ObSEArray<const ObDmlTableInfo *, 4> tables;
    const auto &dml = static_cast<const ObDelUpdStmt &>(statement);
    if (OB_FAIL(dml.get_dml_table_infos(tables))) { return ret; }
    for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
      const ObAssignments *assignments = nullptr;
      if (tables.at(i)->table_type_ == ObDmlTableType::UPDATE_TABLE) {
        assignments = &static_cast<const ObUpdateTableInfo *>(tables.at(i))->assignments_;
      } else if (tables.at(i)->table_type_ == ObDmlTableType::INSERT_TABLE) {
        assignments = &static_cast<const ObInsertTableInfo *>(tables.at(i))->assignments_;
      }
      for (int64_t j = 0; OB_SUCC(ret) && assignments != nullptr && j < assignments->count(); ++j) {
        if (!assignments->at(j).is_implicit_) { ret = expressions.push_back(assignments->at(j).expr_); }
      }
    }
  }
  if (OB_SUCC(ret)) { ret = ObRawExprUtils::extract_column_exprs(expressions, references); }
  for (int64_t i = 0; OB_SUCC(ret) && i < references.count(); ++i) {
    const auto *reference = static_cast<const ObColumnRefRawExpr *>(references.at(i));
    const ColumnItem *item = statement.get_column_item_by_id(reference->get_table_id(), reference->get_column_id());
    if (item == nullptr || !reference->is_ai_column()) { continue; }
    const ColumnItem &column = *item;
    const TableItem *table = statement.get_table_item_by_id(column.table_id_);
    const TableItem *base = statement.get_table_item_by_id(column.base_tid_);
    if (base == nullptr) { base = table; }
    // DML rewrites base_tid_ to a statement-local table ID. Resolve that alias
    // through TableItem before recording a schema ID in the cached plan.
    const uint64_t table_id = base != nullptr && base->get_base_table_item().is_basic_table()
                           ? base->get_base_table_item().ref_id_ : column.base_tid_;
    const uint64_t column_id = column.base_cid_ != OB_INVALID_ID ? column.base_cid_ : column.column_id_;
    if (table_id == OB_INVALID_ID) { continue; }
    bool duplicate = false;
    for (int64_t j = 0; j + 1 < plan.get_ai_column_references().count(); j += 2) {
      duplicate = duplicate || (plan.get_ai_column_references().at(j) == table_id
                             && plan.get_ai_column_references().at(j+1) == column_id);
    }
    if (!duplicate) {
      if (OB_FAIL(plan.get_ai_column_references().push_back(table_id))) {
      } else { ret = plan.get_ai_column_references().push_back(column_id); }
      plan.set_has_nested_sql(true);
    }
  }
  ObSEArray<ObSelectStmt *, 4> children;
  if (OB_SUCC(ret) && OB_SUCC(ret = statement.get_child_stmts(children))) {
    for (int64_t i = 0; OB_SUCC(ret) && i < children.count(); ++i) {
      if (children.at(i) == nullptr) { continue; }
      ObSEArray<int64_t, 8> outputs;
      bool derived = false;
      for (int64_t j = 0; OB_SUCC(ret) && j < statement.get_table_items().count(); ++j) {
        const TableItem *table = statement.get_table_items().at(j);
        if (table == nullptr || table->ref_query_ != children.at(i)) { continue; }
        derived = true;
        for (int64_t k = 0; OB_SUCC(ret) && k < references.count(); ++k) {
          const auto *reference = static_cast<const ObColumnRefRawExpr *>(references.at(k));
          if (reference->get_table_id() == table->table_id_) {
            const int64_t index = reference->get_column_id() - OB_APP_MIN_COLUMN_ID;
            if (!has_exist_in_array(outputs, index)) { ret = outputs.push_back(index); }
          }
        }
      }
      if (OB_SUCC(ret)) {
        ret = SMART_CALL(collect_ai_references(*children.at(i), plan, derived ? &outputs : nullptr));
      }
    }
  }
  return ret;
}

int AIColumnSQL::collect_references(const ObDMLStmt &statement, ObPhysicalPlan &plan)
{
  return collect_ai_references(statement, plan);
}

namespace {
// A statement-local inner connection shares the business transaction, including
// uncommitted changes. Restore its session before releasing an owned connection.
class NestedSQL
{
public:
  explicit NestedSQL(ObSQLSessionInfo &session) : session_(session) {}
  ~NestedSQL()
  {
    if (begun_) { (void)connection_->end_nested_session(saved_session_, saved_connection_); }
    if (owned_) { session_.set_inner_conn(nullptr); }
  }
  int open()
  {
    int ret = OB_SUCCESS;
    if (session_.get_inner_conn() == nullptr) {
      if (OB_SUCC(ret = query::ObInnerSQLConnectionAccess::create_connection_with_external_session(&session_, guard_))) {
        session_.set_inner_conn(guard_.get_ptr());
        owned_ = true;
      }
    }
    if (OB_SUCC(ret)) {
      connection_ = as_inner_sql_connection(session_.get_inner_conn());
      if (connection_ == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_SUCC(ret = connection_->begin_nested_session(saved_session_, saved_connection_, true))) {
        begun_ = true;
      }
    }
    return ret;
  }
  int close()
  {
    int ret = OB_SUCCESS;
    if (begun_) { begun_ = false; ret = connection_->end_nested_session(saved_session_, saved_connection_); }
    return ret;
  }
  ObIInnerSQLConnection *connection_ = nullptr;
private:
  ObSQLSessionInfo &session_;
  sqlclient::ObISQLConnectionGuard guard_;
  ObSQLSessionInfo::StmtSavedValue saved_session_;
  ObIInnerSQLConnection::SavedValue saved_connection_;
  bool owned_ = false;
  bool begun_ = false;
};

int table_name(const ObTableSchema &table, ObSqlString &name)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  ObSchemaGetterGuard guard;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_database_schema(table.get_database_id(), database))) {
  } else if (database == nullptr) { ret = OB_ERR_BAD_DATABASE; }
  else if (OB_FAIL(AIColumnDML::append_identifier(name, database->get_database_name_str()))) {
  } else if (OB_FAIL(name.append("."))) {
  } else { ret = AIColumnDML::append_identifier(name, table.get_table_name_str()); }
  return ret;
}

// Preserve quoted identifiers, literals and comments. Only a bare identifier
// named status denotes the retry pseudo column; `status` is a business column.
int retry_predicate(const ObString &input, const ObString &state_name, bool no_backslash, ObSqlString &output)
{
  int ret = OB_SUCCESS;
  const char *text = input.ptr();
  for (int64_t i = 0; OB_SUCC(ret) && i < input.length();) {
    const int64_t begin = i;
    const char c = text[i++];
    if (c == '\'' || c == '"' || c == '`') {
      while (i < input.length()) {
        const char next = text[i++];
        if (next == '\\' && c != '`' && !no_backslash && i < input.length()) { ++i; }
        else if (next == c) {
          if (i < input.length() && text[i] == c) { ++i; }
          else { break; }
        }
      }
      ret = output.append(text + begin, i - begin);
    } else if (c == '#' || (c == '-' && i + 1 < input.length() && text[i] == '-'
                && static_cast<unsigned char>(text[i+1]) <= ' ')) {
      while (i < input.length() && text[i] != '\n') { ++i; }
      ret = output.append(text + begin, i - begin);
    } else if (c == '/' && i < input.length() && text[i] == '*') {
      ++i;
      while (i + 1 < input.length() && !(text[i] == '*' && text[i+1] == '/')) { ++i; }
      i = MIN(i + 2, input.length());
      ret = output.append(text + begin, i - begin);
    } else if (isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'
                || static_cast<unsigned char>(c) >= 128) {
      while (i < input.length() && (isalnum(static_cast<unsigned char>(text[i]))
          || text[i] == '_' || text[i] == '$' || static_cast<unsigned char>(text[i]) >= 128)) { ++i; }
      const ObString token(i - begin, text + begin);
      int64_t previous = begin - 1, next = i;
      while (previous >= 0 && isspace(static_cast<unsigned char>(text[previous]))) { --previous; }
      while (next < input.length() && isspace(static_cast<unsigned char>(text[next]))) { ++next; }
      const bool qualified = (previous >= 0 && text[previous] == '.')
                          || (next < input.length() && text[next] == '.');
      const bool function = next < input.length() && text[next] == '(';
      if (!qualified && !function && token.case_compare("status") == 0) {
        if (OB_FAIL(output.append("(CASE "))) {
        } else if (OB_FAIL(AIColumnDML::append_identifier(output, state_name))) {
        } else { ret = output.append(" WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' ELSE 'PENDING' END)"); }
      } else { ret = output.append(text + begin, i - begin); }
    } else { ret = output.append(text + begin, 1); }
  }
  return ret;
}
}

int AIColumnSQL::check_read(ObExecContext &context, const ObPhysicalPlan &plan)
{
  int ret = OB_SUCCESS;
  if (AIColumnWriteScope::active() || plan.get_stmt_type() == stmt::T_EXPLAIN
      || plan.get_ai_column_references().empty()) { return ret; }
  ObObj strict;
  ObSQLSessionInfo *session = context.get_my_session();
  if (session == nullptr || context.get_sql_ctx() == nullptr || context.get_sql_ctx()->schema_guard_ == nullptr) {
    return OB_ERR_UNEXPECTED;
  }
  if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, strict))) { return ret; }
  if (strict.get_int() == 0) { return ret; }
  ObSchemaGetterGuard guard;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) { return ret; }
  NestedSQL nested(*session);
  AIColumnNestedSQLScope scope(context);
  if (OB_FAIL(nested.open())) { return ret; }
  for (int64_t i = 0; OB_SUCC(ret) && i + 1 < plan.get_ai_column_references().count(); i += 2) {
    const uint64_t table_id = plan.get_ai_column_references().at(i);
    const uint64_t column_id = plan.get_ai_column_references().at(i+1);
    const ObTableSchema *table = nullptr;
    ObSqlString name, query;
    ObArenaAllocator allocator("AIColumnRead");
    AIColumnInfo info(allocator);
    if (OB_FAIL(guard.get_table_schema(table_id, table))) {
    } else if (table == nullptr) { ret = OB_TABLE_NOT_EXIST; }
    else if (table->get_column_schema(column_id) == nullptr) { ret = OB_SCHEMA_ERROR; }
    else if (OB_FAIL(info.from_json(table->get_column_schema(column_id)->get_ai_column_info_str()))) {
    } else if (OB_FAIL(table_name(*table, name))) {
    } else if (OB_FAIL(query.append_fmt("SELECT /*+ opt_param('hidden_column_visible','true') */ 1 FROM %s WHERE ", name.ptr()))) {
    } else if (OB_FAIL(AIColumnDML::append_column(query, *table, info.state_column_id_))) {
    } else if (OB_FAIL(query.append(" IS NULL OR "))) {
    } else if (OB_FAIL(AIColumnDML::append_column(query, *table, info.state_column_id_))) {
    } else if (OB_FAIL(query.append("<>1 LIMIT 1"))) {
    } else {
      ObISQLClient::ReadResult result;
      if (OB_FAIL(nested.connection_->execute_read(query.string(), result))) {
      } else if (OB_SUCC(ret = result.get_result()->next())) {
        ret = OB_AI_COLUMN_NOT_READY;
        LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, table_id, column_id);
      } else if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    }
  }
  const int restore_ret = nested.close();
  if (OB_SUCC(ret)) { ret = restore_ret; }
  return ret;
}

int AIColumnSQL::retry(ObExecContext &context, const ObTableSchema &table,
                       uint64_t column_id, const ObString &predicate)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = table.get_column_schema(column_id);
  if (column == nullptr || !column->is_ai_column()) { return OB_NOT_SUPPORTED; }
  ObSqlString name, update, condition;
  ObArenaAllocator allocator("AIColumnRetry");
  AIColumnInfo info(allocator);
  const ObColumnSchemaV2 *state = nullptr;
  if (OB_FAIL(info.from_json(column->get_ai_column_info_str()))) {
  } else if ((state = table.get_column_schema(info.state_column_id_)) == nullptr) { ret = OB_SCHEMA_ERROR; }
  else if (OB_FAIL(table_name(table, name))) {
  } else if (OB_FAIL(retry_predicate(predicate, state->get_column_name_str(),
                      (context.get_my_session()->get_sql_mode() & SMO_NO_BACKSLASH_ESCAPES) != 0, condition))) {
  } else if (OB_FAIL(update.append_fmt("UPDATE /*+ opt_param('hidden_column_visible','true') */ %s SET ", name.ptr()))) {
  } else if (OB_FAIL(AIColumnDML::append_identifier(update, column->get_column_name_str()))) {
  } else if (OB_FAIL(update.append("=NULL"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_column_count(); ++i) {
    const ObColumnSchemaV2 &ordinary = *table.get_column_schema_by_idx(i);
    if (!ordinary.is_on_update_current_timestamp() || ordinary.is_ai_column()) { continue; }
    if (OB_FAIL(update.append(","))) {
    } else if (OB_FAIL(AIColumnDML::append_identifier(update, ordinary.get_column_name_str()))) {
    } else if (OB_FAIL(update.append("="))) {
    } else { ret = AIColumnDML::append_identifier(update, ordinary.get_column_name_str()); }
  }
  if (OB_SUCC(ret) && !condition.empty()) { ret = update.append_fmt(" WHERE %s", condition.ptr()); }
  if (OB_SUCC(ret)) {
    AIColumnWriteScope scope(column_id);
    ObSQLSessionInfo *session = context.get_my_session();
    ObBasicSessionInfo::UserScopeGuard user_scope(session->get_sql_scope_flags());
    sqlclient::ObISQLConnectionGuard connection;
    int64_t affected = 0;
    bool previous_autocommit = false;
    if (OB_FAIL(session->get_autocommit(previous_autocommit))) {
    } else if (!previous_autocommit && OB_FAIL(session->set_autocommit(true))) {
    } else if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, connection))) {
    } else if (connection.get_ptr() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(connection.get_ptr()->execute_write(update.string(), affected, true))) {
      LOG_ERROR("AI column retry update failed", K(ret));
    }
    if (!previous_autocommit) {
      const int restore_ret = session->set_autocommit(false);
      if (OB_SUCC(ret)) { ret = restore_ret; }
    }
  }
  return ret;
}
} }
