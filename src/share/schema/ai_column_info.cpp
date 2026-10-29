/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "share/schema/ai_column_info.h"
#include "share/schema/ob_table_schema.h"
#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_parse.h"
#include "lib/alloc/alloc_struct.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
using namespace common;

bool has_ai_columns(const ObTableSchema &table)
{
  bool found = false;
  for (int64_t i = 0; !found && i < table.get_column_count(); ++i) {
    found = table.get_column_schema_by_idx(i)->is_ai_column();
  }
  return found;
}

int AIColumnInfo::from_json(const ObString &json)
{
  int ret = OB_SUCCESS;
  ObJsonNode *root = nullptr;
  if (OB_FAIL(ObJsonParser::get_tree(&allocator_, json, root))) {
  } else if (root == nullptr || root->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const ObJsonObject &object = *static_cast<ObJsonObject *>(root);
    auto string_value = [&](const char *key, ObString &value) -> int {
      ObJsonNode *node = object.get_value(key);
      if (node == nullptr || node->json_type() != ObJsonNodeType::J_STRING) {
        return OB_INVALID_ARGUMENT;
      }
      value = static_cast<ObJsonString *>(node)->value();
      return OB_SUCCESS;
    };
    auto integer_value = [&](const char *key, int64_t &value) -> int {
      ObJsonNode *node = object.get_value(key);
      if (node == nullptr || node->json_type() != ObJsonNodeType::J_INT) {
        return OB_INVALID_ARGUMENT;
      }
      value = static_cast<ObJsonInt *>(node)->value();
      return OB_SUCCESS;
    };
    int64_t state = 0, request = 0, spec = 0;
    if (OB_FAIL(string_value("expression", expression_))) {
    } else if (OB_FAIL(string_value("normalized_expr", normalized_expr_))) {
    } else if (OB_FAIL(string_value("system_prompt", system_prompt_))) {
    } else if (OB_FAIL(integer_value("state_column_id", state))) {
    } else if (OB_FAIL(integer_value("request_column_id", request))) {
    } else if (OB_FAIL(integer_value("result_spec_column_id", spec))) {
    } else if (OB_FAIL(integer_value("definition_version", definition_version_))) {
    } else if (OB_FAIL(integer_value("max_retries", max_retries_))) {
    } else if (state < 0 || request < 0 || spec < 0 || definition_version_ <= 0
               || max_retries_ < 0 || max_retries_ > 100) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      state_column_id_ = state;
      request_column_id_ = request;
      result_spec_column_id_ = spec;
      dependencies_.reset();
      ObJsonNode *node = object.get_value("dependencies");
      if (node == nullptr || node->json_type() != ObJsonNodeType::J_ARRAY) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        ObJsonArray *array = static_cast<ObJsonArray *>(node);
        for (int64_t i = 0; OB_SUCC(ret) && i < array->element_count(); ++i) {
          ObJsonNode *entry = array->get_value(i);
          if (entry == nullptr || entry->json_type() != ObJsonNodeType::J_INT
              || static_cast<ObJsonInt *>(entry)->value() <= 0) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            ret = dependencies_.push_back(static_cast<ObJsonInt *>(entry)->value());
          }
        }
      }
    }
  }
  return ret;
}

int AIColumnInfo::to_json(ObString &json) const
{
  int ret = OB_SUCCESS;
  ObJsonObject root(&allocator_);
  auto add_string = [&](const char *key, const ObString &value) -> int {
    ObJsonString *node = OB_NEWx(ObJsonString, &allocator_, value);
    return node == nullptr ? OB_ALLOCATE_MEMORY_FAILED : root.add(key, node);
  };
  auto add_integer = [&](const char *key, const int64_t value) -> int {
    ObJsonInt *node = OB_NEWx(ObJsonInt, &allocator_, value);
    return node == nullptr ? OB_ALLOCATE_MEMORY_FAILED : root.add(key, node);
  };
  if (OB_FAIL(add_string("expression", expression_))) {
  } else if (OB_FAIL(add_string("normalized_expr", normalized_expr_))) {
  } else if (OB_FAIL(add_string("system_prompt", system_prompt_))) {
  } else if (OB_FAIL(add_integer("state_column_id", state_column_id_))) {
  } else if (OB_FAIL(add_integer("request_column_id", request_column_id_))) {
  } else if (OB_FAIL(add_integer("result_spec_column_id", result_spec_column_id_))) {
  } else if (OB_FAIL(add_integer("definition_version", definition_version_))) {
  } else if (OB_FAIL(add_integer("max_retries", max_retries_))) {
  }
  ObJsonArray *dependencies = OB_NEWx(ObJsonArray, &allocator_, &allocator_);
  if (OB_SUCC(ret) && dependencies == nullptr) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < dependencies_.count(); ++i) {
    ObJsonInt *node = OB_NEWx(ObJsonInt, &allocator_, dependencies_.at(i));
    if (node == nullptr) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ret = dependencies->append(node);
    }
  }
  if (OB_SUCC(ret) && OB_SUCC(ret = root.add("dependencies", dependencies))) {
    ObJsonBuffer buffer(&allocator_);
    if (OB_SUCC(ret = root.print(buffer, false))) {
      ret = ob_write_string(allocator_, buffer.string(), json);
    }
  }
  return ret;
}

int AIColumnInfo::remap_ids(const hash::ObHashMap<uint64_t, uint64_t> &ids)
{
  auto remap = [&](uint64_t &id) {
    uint64_t mapped = 0;
    const int ret = ids.get_refactored(id, mapped);
    if (ret == OB_SUCCESS) {
      id = mapped;
    }
    return ret == OB_HASH_NOT_EXIST ? OB_SUCCESS : ret;
  };
  int ret = remap(state_column_id_);
  if (OB_SUCC(ret)) { ret = remap(request_column_id_); }
  if (OB_SUCC(ret)) { ret = remap(result_spec_column_id_); }
  for (int64_t i = 0; OB_SUCC(ret) && i < dependencies_.count(); ++i) {
    ret = remap(dependencies_.at(i));
  }
  return ret;
}
}
}
}
