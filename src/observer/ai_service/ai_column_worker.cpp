/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ai_column_worker.h"
#include "share/ob_server_struct.h"
#include "observer/ob_inner_sql_result.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ai_column_info.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "query/api/query/session/ob_inner_sql_connection_access.h"
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/expr/ob_expr_ai/ai_column_evaluation.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/expr/ob_expr_sql_udt_utils.h"
#include <openssl/rand.h>
#include <unistd.h>

namespace oceanbase { namespace observer {
using namespace common;
using namespace share::schema;
using sql::AIColumnDML;

namespace {
enum TaskState { PENDING = 0, RUNNING = 1, SUCCEEDED = 2, RETRY_WAIT = 3, FAILED = 4 };
struct Task
{
  uint64_t column_id_ = 0;
  int64_t version_ = 0;
  int64_t state_ = 0;
  int64_t retries_ = 0;
  ObString predicate_;
  ObString payload_;
  bool null_payload_ = true;
};

int read_id(sqlclient::ObMySQLResult &result, int64_t index, uint64_t &id)
{
  int64_t value = 0;
  int ret = result.get_int(index, value);
  if (OB_SUCC(ret)) {
    if (value <= 0) { ret = OB_INVALID_ARGUMENT; }
    else { id = static_cast<uint64_t>(value); }
  }
  return ret;
}

int read_text(sqlclient::ObMySQLResult &result, int64_t index, ObIAllocator &allocator,
              ObString &text, bool &is_null)
{
  int ret = OB_SUCCESS;
  ObObj object;
  ObString temporary;
  if (OB_FAIL(result.get_obj(index, object))) {
  } else if (FALSE_IT(is_null = object.is_null())) {
  } else if (!is_null) {
    auto &native = static_cast<ObInnerSQLResult &>(result);
    if (object.is_collection_sql_type()) {
      if (OB_SUCC(ret = sql::ObSqlUdtUtils::convert_result_for_client(object, native.result_set()))) {
        temporary = object.get_string();
      }
    } else if (OB_FAIL(sql::ObTextStringHelper::read_real_string_data(
                  native.result_set().get_exec_context(), &allocator, object, temporary))) {
    }
    if (OB_SUCC(ret)) { ret = ob_write_string(allocator, temporary, text); }
  }
  return ret;
}

int uuid(ObIAllocator &allocator, ObString &value)
{
  unsigned char bytes[16];
  char buffer[32];
  const char *digits = "0123456789abcdef";
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) { return OB_ERR_UNEXPECTED; }
  for (int i = 0; i < 16; ++i) { buffer[2*i] = digits[bytes[i] >> 4]; buffer[2*i+1] = digits[bytes[i] & 15]; }
  return ob_write_string(allocator, ObString(sizeof(buffer), buffer), value);
}

int group_filter(ObSqlString &query, uint64_t table_id, const ObString &request,
                  const ObString *owner = nullptr)
{
  int ret = query.append_fmt("table_id=%lu AND request_id=", table_id);
  if (OB_SUCC(ret)) { ret = AIColumnDML::append_string(query, request); }
  if (OB_SUCC(ret) && owner != nullptr) {
    if (OB_SUCC(ret = query.append(" AND owner="))) { ret = AIColumnDML::append_string(query, *owner); }
    if (OB_SUCC(ret)) { ret = query.append(" AND lease_until>NOW(6)"); }
  }
  return ret;
}

int qualified_table(ObSchemaGetterGuard &guard, const ObTableSchema &table, ObSqlString &name)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(guard.get_database_schema(table.get_database_id(), database))) {
  } else if (database == nullptr) { ret = OB_ERR_BAD_DATABASE; }
  else if (OB_FAIL(AIColumnDML::append_identifier(name, database->get_database_name_str()))) {
  } else if (OB_FAIL(name.append("."))) {
  } else { ret = AIColumnDML::append_identifier(name, table.get_table_name_str()); }
  return ret;
}

int load_tasks(ObISQLClient &client, uint64_t table_id, const ObString &request,
                const ObString &owner, bool lock, ObIAllocator &allocator, ObIArray<Task> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString query;
  ObISQLClient::ReadResult result;
  if (OB_FAIL(query.append("SELECT column_id,definition_version,state,retry_count,rowkey_predicate,result_payload "
                           "FROM oceanbase.__all_ai_column_task WHERE "))) {
  } else if (OB_FAIL(group_filter(query, table_id, request, &owner))) {
  } else if (OB_FAIL(query.append(lock ? " ORDER BY column_id FOR UPDATE" : " ORDER BY column_id"))) {
  } else if (OB_FAIL(client.read(result, query.ptr()))) {
  } else {
    sqlclient::ObMySQLResult *rows = result.get_result();
    while (OB_SUCC(ret) && OB_SUCC(ret = rows->next())) {
      Task task;
      bool ignored = false;
      if (OB_FAIL(read_id(*rows, 0, task.column_id_))) {
      } else if (OB_FAIL(rows->get_int(1, task.version_))) {
      } else if (OB_FAIL(rows->get_int(2, task.state_))) {
      } else if (OB_FAIL(rows->get_int(3, task.retries_))) {
      } else if (OB_FAIL(read_text(*rows, 4, allocator, task.predicate_, ignored))) {
      } else if (OB_FAIL(read_text(*rows, 5, allocator, task.payload_, task.null_payload_))) {
      } else { ret = tasks.push_back(task); }
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  }
  return ret;
}

int drop_task(uint64_t table_id, const ObString &request, const ObString &owner, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  ObSqlString query;
  if (OB_FAIL(query.append("DELETE FROM oceanbase.__all_ai_column_task WHERE "))) {
  } else if (OB_FAIL(group_filter(query, table_id, request, &owner))) {
  } else if (OB_FAIL(query.append_fmt(" AND column_id=%lu", column_id))) {
  } else { ret = GCTX.sql_proxy_->write(query.ptr(), affected); }
  return ret;
}

int evaluate(uint64_t table_id, const ObString &request, const ObString &owner, const Task &task)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColumnEval");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  AIColumnInfo info(allocator);
  ObSqlString name, query;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table))) {
  } else if (table == nullptr || (column = table->get_column_schema(task.column_id_)) == nullptr
             || !column->is_ai_column()) {
    return drop_task(table_id, request, owner, task.column_id_);
  } else if (OB_FAIL(info.from_json(column->get_ai_column_info_str()))) {
  } else if (info.definition_version_ != task.version_) {
    return drop_task(table_id, request, owner, task.column_id_);
  } else if (OB_FAIL(qualified_table(guard, *table, name))) {
  } else if (OB_FAIL(query.append_fmt("SELECT /*+ query_timeout(7000000) opt_param('hidden_column_visible','true') */ "
        "%s(%.*s)%s FROM %s WHERE (%.*s)", column->is_collection() ? "" : "CAST(",
        info.normalized_expr_.length(), info.normalized_expr_.ptr(),
        column->is_collection() ? "" : " AS CHAR CHARACTER SET utf8mb4)", name.ptr(),
        task.predicate_.length(), task.predicate_.ptr()))) {
  } else if (OB_FAIL(query.append(" AND "))) {
  } else if (OB_FAIL(AIColumnDML::append_column(query, *table, info.request_column_id_))) {
  } else if (OB_FAIL(query.append("="))) {
  } else if (OB_FAIL(AIColumnDML::append_string(query, request))) {
  } else if (OB_FAIL(query.append(" AND "))) {
  } else if (OB_FAIL(AIColumnDML::append_column(query, *table, info.result_spec_column_id_))) {
  } else if (OB_FAIL(query.append_fmt("=%ld AND ", task.version_))) {
  } else if (OB_FAIL(AIColumnDML::append_column(query, *table, info.state_column_id_))) {
  } else { ret = query.append("=0"); }
  if (OB_FAIL(ret)) { return ret; }

  ObString payload;
  bool is_null = true, found = false;
  {
    // A dedicated connection permits strict conversion without changing any
    // business session. No transaction or business-row lock spans the HTTP call.
    ObSingleConnectionProxy connection;
    if (OB_SUCC(ret = connection.connect(0, GCTX.sql_proxy_))) {
      sql::ObSQLSessionInfo *session = oceanbase::query::ObInnerSQLConnectionAccess::get_session(connection.get_connection());
      if (session == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        const ObSQLMode previous_mode = session->get_sql_mode();
        ObObj mode;
        mode.set_uint64(previous_mode | SMO_STRICT_ALL_TABLES);
        if (OB_SUCC(ret = session->update_sys_variable(share::SYS_VAR_SQL_MODE, mode))) {
          sql::AIColumnEvaluationScope scope(*column);
          ObISQLClient::ReadResult result;
          if (OB_FAIL(connection.read(result, query.ptr()))) {
          } else if (OB_FAIL(result.get_result()->next())) {
            if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
          } else {
            found = true;
            ret = read_text(*result.get_result(), 0, allocator, payload, is_null);
          }
        }
        mode.set_uint64(previous_mode);
        int restore_ret = session->update_sys_variable(share::SYS_VAR_SQL_MODE, mode);
        if (OB_SUCC(ret)) { ret = restore_ret; }
      }
    }
  }
  if (OB_SUCC(ret) && !found) { return drop_task(table_id, request, owner, task.column_id_); }

  const int evaluation_error = ret;
  if (evaluation_error != OB_SUCCESS) { LOG_WARN("AI column evaluation failed", K(evaluation_error), K(table_id), K(task.column_id_)); }
  ret = OB_SUCCESS;
  ObSqlString save;
  int64_t affected = 0;
  if (evaluation_error == OB_SUCCESS) {
    if (OB_FAIL(save.append("UPDATE oceanbase.__all_ai_column_task SET state=2,last_error=0,result_payload="))) {
    } else if (is_null) { ret = save.append("NULL"); }
    else { ret = AIColumnDML::append_string(save, payload); }
  } else {
    const bool retry = task.retries_ < info.max_retries_;
    const int64_t delay = 1LL << MIN(task.retries_, 5L);
    ret = save.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%d,retry_count=retry_count+1,"
        "last_error=%d,result_payload=NULL,eligible_ts=DATE_ADD(NOW(6),INTERVAL %ld SECOND)",
        retry ? RETRY_WAIT : FAILED, evaluation_error, delay);
  }
  if (OB_SUCC(ret) && OB_SUCC(ret = save.append(" WHERE "))
      && OB_SUCC(ret = group_filter(save, table_id, request, &owner))
      && OB_SUCC(ret = save.append_fmt(" AND column_id=%lu AND state=1", task.column_id_))) {
    ret = GCTX.sql_proxy_->write(save.ptr(), affected);
  }
  return ret;
}

int publish(uint64_t table_id, const ObString &request, const ObString &owner,
              const ObIArray<Task> &observed)
{
  int ret = OB_SUCCESS;
  if (observed.empty()) { return ret; }
  ObArenaAllocator allocator("AIColumnPublish");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  ObSqlString name, lock, update, remove;
  ObMySQLTransaction transaction;
  int64_t affected = 0;
  bool exists = false;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table))) {
  } else if (table == nullptr) {
    for (int64_t i = 0; OB_SUCC(ret) && i < observed.count(); ++i) {
      ret = drop_task(table_id, request, owner, observed.at(i).column_id_);
    }
    return ret;
  } else if (OB_FAIL(qualified_table(guard, *table, name))) {
  } else if (OB_FAIL(transaction.start(GCTX.sql_proxy_))) {
  } else if (OB_FAIL(lock.append_fmt("SELECT /*+ query_timeout(5000000) opt_param('hidden_column_visible','true') */ "
             "1 FROM %s WHERE (%.*s) FOR UPDATE",
             name.ptr(), observed.at(0).predicate_.length(), observed.at(0).predicate_.ptr()))) {
  } else {
    // Business DML locks the row before modifying its tasks. Use the same order.
    ObISQLClient::ReadResult result;
    if (OB_FAIL(transaction.read(result, lock.ptr()))) {
    } else if (OB_SUCC(ret = result.get_result()->next())) { exists = true; }
    else if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  }
  ObSEArray<Task, 4> tasks;
  if (OB_SUCC(ret)) { ret = load_tasks(transaction, table_id, request, owner, true, allocator, tasks); }
  ObSchemaGetterGuard publication_guard;
  if (OB_SUCC(ret) && exists) {
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(publication_guard))) {
    } else { ret = publication_guard.get_table_schema(table_id, table); }
    if (OB_SUCC(ret) && table == nullptr) { exists = false; }
    if (OB_SUCC(ret) && exists) {
      name.reset();
      ret = qualified_table(publication_guard, *table, name);
    }
  }
  bool terminal = !tasks.empty();
  for (int64_t i = 0; i < tasks.count(); ++i) {
    terminal = terminal && (tasks.at(i).state_ == SUCCEEDED || tasks.at(i).state_ == FAILED);
  }
  if (OB_SUCC(ret) && terminal && exists) {
    if (OB_FAIL(update.append_fmt("UPDATE /*+ query_timeout(5000000) opt_param('hidden_column_visible','true') */ %s SET ", name.ptr()))) {
    }
    ObSqlString condition;
    int64_t count = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      const Task &task = tasks.at(i);
      const ObColumnSchemaV2 *column = table->get_column_schema(task.column_id_);
      if (column == nullptr || !column->is_ai_column()) { continue; }
      AIColumnInfo info(allocator);
      if (OB_FAIL(info.from_json(column->get_ai_column_info_str()))) {
      } else if (info.definition_version_ != task.version_) { continue; }
      else if (count++ != 0 && OB_FAIL(update.append(","))) {
      } else if (OB_FAIL(AIColumnDML::append_identifier(update, column->get_column_name_str()))) {
      } else if (OB_FAIL(update.append("="))) {
      } else if (task.state_ == FAILED || task.null_payload_) { ret = update.append("NULL"); }
      else {
        if (OB_SUCC(ret = update.append("CONVERT("))
            && OB_SUCC(ret = AIColumnDML::append_string(update, task.payload_))) {
          ret = update.append(" USING utf8mb4)");
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(update.append(","))) {
        } else if (OB_FAIL(AIColumnDML::append_column(update, *table, info.state_column_id_))) {
        } else { ret = update.append_fmt("=%d", task.state_ == SUCCEEDED ? 1 : 2); }
      }
      if (OB_SUCC(ret)) { ret = condition.append(" AND "); }
      if (OB_SUCC(ret)) { ret = AIColumnDML::append_column(condition, *table, info.request_column_id_); }
      if (OB_SUCC(ret)) { ret = condition.append("="); }
      if (OB_SUCC(ret)) { ret = AIColumnDML::append_string(condition, request); }
      if (OB_SUCC(ret)) { ret = condition.append(" AND "); }
      if (OB_SUCC(ret)) { ret = AIColumnDML::append_column(condition, *table, info.result_spec_column_id_); }
      if (OB_SUCC(ret)) { ret = condition.append_fmt("=%ld", task.version_); }
    }
    if (OB_SUCC(ret) && count > 0) {
      // Publishing generated values must not advance business ON UPDATE fields;
      // they may themselves be inputs of an AI expression.
      for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
        const ObColumnSchemaV2 &column = *table->get_column_schema_by_idx(i);
        if (!column.is_on_update_current_timestamp() || column.is_ai_column()) { continue; }
        if (OB_FAIL(update.append(","))) {
        } else if (OB_FAIL(AIColumnDML::append_identifier(update, column.get_column_name_str()))) {
        } else if (OB_FAIL(update.append("="))) {
        } else { ret = AIColumnDML::append_identifier(update, column.get_column_name_str()); }
      }
    }
    if (OB_SUCC(ret) && count > 0) {
      if (OB_FAIL(update.append_fmt(" WHERE (%.*s)%s", tasks.at(0).predicate_.length(),
                                    tasks.at(0).predicate_.ptr(), condition.ptr()))) {
      } else {
        sql::AIColumnWriteScope scope;
        ret = transaction.write(update.ptr(), affected);
      }
    }
  }
  if (OB_SUCC(ret) && (terminal || !exists)) {
    if (OB_FAIL(remove.append("DELETE FROM oceanbase.__all_ai_column_task WHERE "))) {
    } else if (OB_FAIL(group_filter(remove, table_id, request, &owner))) {
    } else { ret = transaction.write(remove.ptr(), affected); }
  }
  if (transaction.is_started()) {
    const int end_ret = transaction.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}

int publication_failed(uint64_t table_id, const ObString &request, const ObString &owner, int error)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColumnFailure");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  ObSEArray<Task, 4> tasks;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table))) {
  } else if (table == nullptr) {
  } else if (OB_FAIL(load_tasks(*GCTX.sql_proxy_, table_id, request, owner, false, allocator, tasks))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      const Task &task = tasks.at(i);
      const ObColumnSchemaV2 *column = table->get_column_schema(task.column_id_);
      if (task.state_ != SUCCEEDED || column == nullptr || !column->is_ai_column()) { continue; }
      AIColumnInfo info(allocator);
      ObSqlString update;
      int64_t affected = 0;
      if (OB_FAIL(info.from_json(column->get_ai_column_info_str()))) {
      } else if (OB_FAIL(update.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%d,"
          "retry_count=retry_count+1,result_payload=NULL,last_error=%d,"
          "eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND) WHERE ",
          task.retries_ < info.max_retries_ ? RETRY_WAIT : FAILED, error))) {
      } else if (OB_FAIL(group_filter(update, table_id, request, &owner))) {
      } else if (OB_FAIL(update.append_fmt(" AND column_id=%lu AND state=2", task.column_id_))) {
      } else { ret = GCTX.sql_proxy_->write(update.ptr(), affected); }
    }
  }
  return ret;
}
} // namespace

int AIColumnWorker::init()
{
  set_run_wrapper(share::server_runtime());
  return set_thread_count(4);
}

void AIColumnWorker::run1()
{
  lib::set_thread_name("AIColumnWorker");
  while (!has_set_stop()) {
    bool found = false;
    int ret = process_one(found);
    if (ret != OB_SUCCESS && REACH_TIME_INTERVAL(1000000)) { LOG_WARN("AI column worker failed", K(ret), K(found)); }
    if (OB_FAIL(ret) || !found) { usleep(100000); }
  }
}

int AIColumnWorker::process_one(bool &found)
{
  int ret = OB_SUCCESS;
  found = false;
  if (GCTX.sql_proxy_ == nullptr || GCTX.schema_service_ == nullptr) { return OB_NOT_INIT; }
  ObArenaAllocator allocator("AIColumnWorker");
  uint64_t table_id = 0;
  ObString request, owner;
  {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(GCTX.sql_proxy_->read(result,
        "SELECT /*+ query_timeout(1000000) */ table_id,request_id FROM oceanbase.__all_ai_column_task "
        "WHERE eligible_ts<=NOW(6) AND (owner='' OR lease_until<NOW(6)) ORDER BY eligible_ts LIMIT 1"))) {
    } else if (OB_FAIL(result.get_result()->next())) {
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    } else {
      bool ignored = false;
      found = true;
      if (OB_FAIL(read_id(*result.get_result(), 0, table_id))) {
      } else { ret = read_text(*result.get_result(), 1, allocator, request, ignored); }
    }
  }
  if (OB_FAIL(ret) || !found) { return ret; }
  ObSqlString claim;
  int64_t affected = 0;
  if (OB_FAIL(uuid(allocator, owner))) {
  } else if (OB_FAIL(claim.append("UPDATE oceanbase.__all_ai_column_task SET owner="))) {
  } else if (OB_FAIL(AIColumnDML::append_string(claim, owner))) {
  } else if (OB_FAIL(claim.append(",lease_until=DATE_ADD(NOW(6),INTERVAL 30 SECOND),"
      "state=CASE WHEN state IN (0,1,3) AND eligible_ts<=NOW(6) THEN 1 ELSE state END WHERE "))) {
  } else if (OB_FAIL(group_filter(claim, table_id, request))) {
  } else if (OB_FAIL(claim.append(" AND (owner='' OR lease_until<NOW(6))"))) {
  } else { ret = GCTX.sql_proxy_->write(claim.ptr(), affected); }
  if (OB_FAIL(ret) || affected == 0) { return ret; }
  ObSEArray<Task, 4> tasks;
  if (OB_SUCC(ret = load_tasks(*GCTX.sql_proxy_, table_id, request, owner, false, allocator, tasks))) {
    for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < tasks.count(); ++i) {
      if (tasks.at(i).state_ != RUNNING) { continue; }
      ObSqlString renew;
      if (OB_FAIL(renew.append("UPDATE oceanbase.__all_ai_column_task SET lease_until=DATE_ADD(NOW(6),INTERVAL 30 SECOND) WHERE "))) {
      } else if (OB_FAIL(group_filter(renew, table_id, request, &owner))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->write(renew.ptr(), affected))) {
      } else if (affected == 0) { break; }
      else { ret = evaluate(table_id, request, owner, tasks.at(i)); }
    }
  }
  if (OB_SUCC(ret)) {
    ret = publish(table_id, request, owner, tasks);
    if (OB_FAIL(ret)) {
      const int publish_error = ret;
      int failure_ret = publication_failed(table_id, request, owner, publish_error);
      if (failure_ret != OB_SUCCESS) { LOG_WARN("failed to persist AI column publication failure", K(failure_ret), K(table_id)); }
    }
  }
  // Release both completed and waiting members. Terminal members remain durable
  // until publication and are revisited after crashes or concurrent invalidation.
  ObSqlString release;
  int release_ret = release.append("UPDATE oceanbase.__all_ai_column_task SET owner='',lease_until=NULL,"
      "eligible_ts=CASE WHEN state IN (2,4) THEN DATE_ADD(NOW(6),INTERVAL 1 SECOND) ELSE eligible_ts END WHERE ");
  if (OB_SUCC(release_ret)) { release_ret = group_filter(release, table_id, request, &owner); }
  if (OB_SUCC(release_ret)) { release_ret = GCTX.sql_proxy_->write(release.ptr(), affected); }
  if (OB_SUCC(ret)) { ret = release_ret; }
  return ret;
}
} }
