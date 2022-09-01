//===- unittests/TimerTest.cpp - Timer tests ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// These are bare-minimum 'smake' tests of the time profiler. Not tested:
//  - multi-threading
//  - 'Total' entries
//  - elision of short or ill-formed entries
//  - detail callback
//  - no calls to now() if profiling is disabled
//  - suppression of contributions to total entries for nested entries
//===----------------------------------------------------------------------===//

#include "llvm/Support/TimeProfiler.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {
void setup() {
  timeTraceProfilerInitialize(/*TimeTraceGranularity=*/0, "test");
}

std::string teardown() {
  std::string json;
  raw_string_ostream os(json);
  timeTraceProfilerWrite(os);
  timeTraceProfilerCleanup();
  return json;
}

TEST(TimeProfiler, Scope_Smoke) {
  setup();

  { TimeTraceScope scope("event", "detail"); }

  std::string json = teardown();
  ASSERT_TRUE(json.find(R"("name":"event")") != std::string::npos);
  ASSERT_TRUE(json.find(R"("detail":"detail")") != std::string::npos);
}

TEST(TimeProfiler, Begin_End_Smoke) {
  setup();

  timeTraceProfilerBegin("event", "detail");
  timeTraceProfilerEnd();

  std::string json = teardown();
  ASSERT_TRUE(json.find(R"("name":"event")") != std::string::npos);
  ASSERT_TRUE(json.find(R"("detail":"detail")") != std::string::npos);
}

} // namespace