//===-- ProcessRunLock.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _WIN32
#include "lldb/Host/ProcessRunLock.h"
#include "lldb/Target/Policy.h"

namespace lldb_private {

ProcessRunLock::ProcessRunLock() {
  int err = ::pthread_rwlock_init(&m_rwlock, nullptr);
  (void)err;
}

ProcessRunLock::~ProcessRunLock() {
  int err = ::pthread_rwlock_destroy(&m_rwlock);
  (void)err;
}

bool ProcessRunLock::ReadTryLock() {
  auto &policy = PolicyStack::GetForCurrentThread().Current();
  if (policy.capabilities.holds_run_lock)
    return !m_running;

  ::pthread_rwlock_rdlock(&m_rwlock);
  if (!m_running) {
    // coverity[missing_unlock]
    return true;
  }
  ::pthread_rwlock_unlock(&m_rwlock);
  return false;
}

bool ProcessRunLock::ReadUnlock() {
  auto &policy = PolicyStack::GetForCurrentThread().Current();
  if (policy.capabilities.holds_run_lock)
    return true;

  return ::pthread_rwlock_unlock(&m_rwlock) == 0;
}

bool ProcessRunLock::ProcessRunLocker::TryLock(ProcessRunLock *lock) {
  if (m_lock) {
    if (m_lock == lock)
      return true;
    Unlock();
  }
  if (lock) {
    if (lock->ReadTryLock()) {
      m_lock = lock;
      // Push a policy so re-entrant ReadTryLock calls from the same
      // thread (e.g. provider Python calling back into SB API) skip
      // the real lock and avoid deadlocking with a pending writer.
      auto policy = PolicyStack::GetForCurrentThread().Current();
      policy.capabilities.holds_run_lock = true;
      PolicyStack::GetForCurrentThread().Push(policy);
      return true;
    }
  }
  return false;
}

void ProcessRunLock::ProcessRunLocker::Unlock() {
  if (m_lock) {
    PolicyStack::GetForCurrentThread().Pop();
    m_lock->ReadUnlock();
    m_lock = nullptr;
  }
}

bool ProcessRunLock::SetRunning() {
  ::pthread_rwlock_wrlock(&m_rwlock);
  bool was_stopped = !m_running;
  m_running = true;
  ::pthread_rwlock_unlock(&m_rwlock);
  return was_stopped;
}

bool ProcessRunLock::SetStopped() {
  ::pthread_rwlock_wrlock(&m_rwlock);
  bool was_running = m_running;
  m_running = false;
  ::pthread_rwlock_unlock(&m_rwlock);
  return was_running;
}

} // namespace lldb_private

#endif
