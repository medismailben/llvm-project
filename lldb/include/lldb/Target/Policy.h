//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_POLICY_H
#define LLDB_TARGET_POLICY_H

#include "lldb/Utility/ThreadLocalPolicyStack.h"

namespace lldb_private {

/// Describes what view of the process a thread should see and what
/// operations it is allowed to perform.
///
/// Frame providers are a public illusion layered on top of the private
/// reality (the unwinder stack). The private state thread manages the
/// stop of that private reality, so the correct view for its logic IS the
/// private reality. The public illusion is only applied once the process
/// has settled and clients query the stopped state.
///
/// This struct is a pure data store -- no business logic. It is pushed
/// onto a per-thread stack (PolicyStack) so that code
/// can consult the current policy instead of comparing host thread
/// identities.
struct Policy {
  /// What view of the process this thread sees.
  enum class View {
    Public,  // Provider-augmented frames, public state, public run lock.
    Private, // Parent (unwinder) frames, private state, private run lock.
  };

  /// What operations this thread is allowed to perform.
  /// Enforced at specific callsites, not by the policy itself.
  struct Capabilities {
    bool can_evaluate_expressions : 1;
    bool stop_others_only : 1;
    bool can_try_all_threads : 1;
    bool can_run_breakpoint_actions : 1;
    bool can_load_frame_providers : 1;
    bool can_run_frame_recognizers : 1;
    bool holds_run_lock : 1;
  };

  View view = View::Public;
  Capabilities capabilities = {
      .can_evaluate_expressions = true,
      .stop_others_only = false,
      .can_try_all_threads = true,
      .can_run_breakpoint_actions = true,
      .can_load_frame_providers = true,
      .can_run_frame_recognizers = true,
      .holds_run_lock = false,
  };

  static Policy PublicState() { return {}; }

  static Policy PrivateStateRunningExpression() {
    return {
        .view = View::Private,
        .capabilities =
            {
                .can_evaluate_expressions = true,
                .stop_others_only = false,
                .can_try_all_threads = true,
                .can_run_breakpoint_actions = true,
                .can_load_frame_providers = false,
                .can_run_frame_recognizers = false,
                .holds_run_lock = false,
            },
    };
  }

  static Policy PrivateState() {
    return {
        .view = View::Private,
        .capabilities =
            {
                .can_evaluate_expressions = true,
                .stop_others_only = false,
                .can_try_all_threads = true,
                .can_run_breakpoint_actions = true,
                .can_load_frame_providers = true,
                .can_run_frame_recognizers = true,
                .holds_run_lock = false,
            },
    };
  }

  static Policy PublicStateRunningExpression() {
    return {
        .view = View::Public,
        .capabilities =
            {
                .can_evaluate_expressions = true,
                .stop_others_only = false,
                .can_try_all_threads = true,
                .can_run_breakpoint_actions = false,
                .can_load_frame_providers = true,
                .can_run_frame_recognizers = true,
                .holds_run_lock = false,
            },
    };
  }
};

using PolicyStack =
    ThreadLocalPolicyStack<Policy>;

} // namespace lldb_private

#endif // LLDB_TARGET_POLICY_H
