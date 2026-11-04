//===-- SharedScopeTest.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Utility/SharedScope.h"
#include "gtest/gtest.h"

#include <memory>
#include <optional>
#include <thread>

using namespace lldb_private;

namespace {
struct TestData {
  int value = 0;
};
using TestScope = SharedScope<TestData>;
} // namespace

TEST(SharedScopeTest, OwnerSeesItsData) {
  TestScope scope;
  EXPECT_EQ(scope.GetDataForCurrentThread(), nullptr);

  auto data = std::make_shared<TestData>();
  {
    TestScope::Guard guard = scope.Enter(data);
    EXPECT_EQ(scope.GetDataForCurrentThread(), data);
    EXPECT_FALSE(scope.IsActiveOnOtherThread());
  }
  EXPECT_EQ(scope.GetDataForCurrentThread(), nullptr);
}

TEST(SharedScopeTest, OtherThreadsDoNotSeeTheData) {
  TestScope scope;
  TestScope::Guard guard = scope.Enter(std::make_shared<TestData>());

  TestScope::DataSP seen;
  bool active_elsewhere = false;
  std::thread([&] {
    seen = scope.GetDataForCurrentThread();
    active_elsewhere = scope.IsActiveOnOtherThread();
  }).join();

  EXPECT_EQ(seen, nullptr);
  EXPECT_TRUE(active_elsewhere);
}

TEST(SharedScopeTest, NestedActivitiesUseTheInnermost) {
  TestScope scope;
  auto outer = std::make_shared<TestData>();
  auto inner = std::make_shared<TestData>();

  TestScope::Guard outer_guard = scope.Enter(outer);
  {
    TestScope::Guard inner_guard = scope.Enter(inner);
    EXPECT_EQ(scope.GetDataForCurrentThread(), inner);
  }
  EXPECT_EQ(scope.GetDataForCurrentThread(), outer);
}

TEST(SharedScopeTest, AdoptingATokenJoinsTheActivity) {
  TestScope scope;
  auto data = std::make_shared<TestData>();
  TestScope::Guard guard = scope.Enter(data);
  TestScope::Token token = guard.Share();

  TestScope::DataSP while_joined;
  TestScope::DataSP after_leaving;
  bool active_elsewhere = true;
  std::thread([&] {
    {
      TestScope::Participation participation = token.Adopt();
      while_joined = scope.GetDataForCurrentThread();
      active_elsewhere = scope.IsActiveOnOtherThread();
    }
    after_leaving = scope.GetDataForCurrentThread();
  }).join();

  EXPECT_EQ(while_joined, data);
  EXPECT_FALSE(active_elsewhere);
  EXPECT_EQ(after_leaving, nullptr);
}

TEST(SharedScopeTest, AdoptingAfterTheActivityClosedDoesNothing) {
  TestScope scope;
  std::optional<TestScope::Guard> guard;
  guard.emplace(scope.Enter(std::make_shared<TestData>()));
  TestScope::Token token = guard->Share();
  guard.reset();

  TestScope::DataSP seen;
  std::thread([&] {
    TestScope::Participation participation = token.Adopt();
    seen = scope.GetDataForCurrentThread();
  }).join();

  EXPECT_EQ(seen, nullptr);
}

TEST(SharedScopeTest, ActivitiesOfDifferentThreadsAreIndependent) {
  TestScope scope;
  auto mine = std::make_shared<TestData>();
  TestScope::Guard guard = scope.Enter(mine);

  TestScope::DataSP theirs_seen;
  std::thread([&] {
    auto theirs = std::make_shared<TestData>();
    TestScope::Guard their_guard = scope.Enter(theirs);
    theirs_seen = scope.GetDataForCurrentThread();
    EXPECT_EQ(theirs_seen, theirs);
    EXPECT_TRUE(scope.IsActiveOnOtherThread());
  }).join();

  // The other thread's activity closed without touching this one.
  EXPECT_EQ(scope.GetDataForCurrentThread(), mine);
  EXPECT_FALSE(scope.IsActiveOnOtherThread());
}
