/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "share/ob_thread_pool.h"

namespace oceanbase { namespace observer {
class AIColumnWorker : public share::ObThreadPool
{
public:
  int init();
private:
  void run1() override;
  int process_one(bool &found);
};
} }
