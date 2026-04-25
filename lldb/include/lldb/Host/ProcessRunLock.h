//===-- ProcessRunLock.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_HOST_PROCESSRUNLOCK_H
#define LLDB_HOST_PROCESSRUNLOCK_H

#include <cstdint>
#include <ctime>

#include "lldb/lldb-defines.h"

/// Enumerations for broadcasting.
namespace lldb_private {

/// \class ProcessRunLock ProcessRunLock.h "lldb/Host/ProcessRunLock.h"
/// A class used to prevent the process from starting while other
/// threads are accessing its data, and prevent access to its data while it is
/// running.

class ProcessRunLock {
public:
  ProcessRunLock();
  ~ProcessRunLock();

  bool ReadTryLock();
  bool ReadUnlock();

  /// Set the process to running. Returns true if the process was stopped.
  /// Return false if the process was running.
  bool SetRunning();

  /// Set the process to stopped. Returns true if the process was running.
  /// Returns false if the process was stopped.
  bool SetStopped();

  class ProcessRunLocker {
  public:
    ProcessRunLocker() = default;
    ProcessRunLocker(ProcessRunLocker &&other) : m_lock(other.m_lock) {
      other.m_lock = nullptr;
    }
    ProcessRunLocker &operator=(ProcessRunLocker &&other) {
      if (this != &other) {
        Unlock();
        m_lock = other.m_lock;
        other.m_lock = nullptr;
      }
      return *this;
    }

    ~ProcessRunLocker() { Unlock(); }

    bool IsLocked() const { return m_lock; }

    // Try to lock the read lock, but only do so if there are no writers.
    // Pushes a policy with holds_run_lock=true so that re-entrant
    // ReadTryLock calls from the same thread skip the real lock.
    bool TryLock(ProcessRunLock *lock);

  protected:
    void Unlock();

    ProcessRunLock *m_lock = nullptr;

  private:
    ProcessRunLocker(const ProcessRunLocker &) = delete;
    const ProcessRunLocker &operator=(const ProcessRunLocker &) = delete;
  };

protected:
  lldb::rwlock_t m_rwlock;
  bool m_running = false;

private:
  ProcessRunLock(const ProcessRunLock &) = delete;
  const ProcessRunLock &operator=(const ProcessRunLock &) = delete;
};

} // namespace lldb_private

#endif // LLDB_HOST_PROCESSRUNLOCK_H
