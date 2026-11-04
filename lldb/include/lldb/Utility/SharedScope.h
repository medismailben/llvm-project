//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_UTILITY_SHAREDSCOPE_H
#define LLDB_UTILITY_SHAREDSCOPE_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace lldb_private {

/// Activities in progress on one shared object, which the threads doing them
/// see differently from every other thread.
///
/// A PolicyStack describes what one thread is doing, and only that thread
/// reads or writes it. A SharedScope describes what is in progress on an
/// object, which any thread may need to ask about. Each activity is a record
/// with the thread that opened it, the threads that joined it, and the data
/// its members use instead of the object's own.
///
/// The owner opens an activity with Enter() and closes it by destroying the
/// returned Guard. Another thread joins it by adopting a Token from the Guard,
/// for as long as the returned Participation lives. Activities of different
/// threads can be open at the same time, and a thread's own activities nest:
/// a thread sees the data of the innermost activity it's a member of.
///
/// The mutex is a leaf: it's only held to read or change the records, never
/// while calling out.
template <typename Data> class SharedScope {
  struct Record {
    Record(std::thread::id owner, std::shared_ptr<Data> data)
        : m_data(std::move(data)) {
      m_members.push_back(owner);
    }

    /// The owner first, then the threads that joined.
    llvm::SmallVector<std::thread::id, 2> m_members;
    std::shared_ptr<Data> m_data;
  };

public:
  using DataSP = std::shared_ptr<Data>;

  class Token;
  class Guard;

  /// A thread's membership in an activity it joined, which ends when this is
  /// destroyed. A default-constructed Participation is not a member of
  /// anything.
  class Participation {
  public:
    Participation() = default;
    Participation(Participation &&other)
        : m_scope(std::exchange(other.m_scope, nullptr)),
          m_record(std::move(other.m_record)), m_thread(other.m_thread) {}
    Participation &operator=(Participation &&) = delete;
    Participation(const Participation &) = delete;
    Participation &operator=(const Participation &) = delete;

    ~Participation() {
      if (m_scope)
        m_scope->Leave(*m_record, m_thread);
    }

  private:
    friend class SharedScope;
    friend class Token;
    Participation(SharedScope &scope, std::shared_ptr<Record> record,
                  std::thread::id thread)
        : m_scope(&scope), m_record(std::move(record)), m_thread(thread) {}

    SharedScope *m_scope = nullptr;
    std::shared_ptr<Record> m_record;
    std::thread::id m_thread;
  };

  /// A handle another thread can adopt to join an activity. It doesn't keep
  /// the activity open: adopting it after the activity has closed does
  /// nothing.
  class Token {
  public:
    Token() = default;

    /// Join the activity on the calling thread until the returned
    /// Participation is destroyed.
    [[nodiscard]] Participation Adopt() const {
      std::shared_ptr<Record> record = m_record.lock();
      if (!record || !m_scope)
        return Participation();
      const std::thread::id self = std::this_thread::get_id();
      if (!m_scope->Join(record, self))
        return Participation();
      return Participation(*m_scope, std::move(record), self);
    }

  private:
    friend class SharedScope;
    friend class Guard;
    Token(SharedScope &scope, std::weak_ptr<Record> record)
        : m_scope(&scope), m_record(std::move(record)) {}

    SharedScope *m_scope = nullptr;
    std::weak_ptr<Record> m_record;
  };

  /// An open activity, closed when this is destroyed. Destroy it on the
  /// thread that opened it, after every thread that joined it has left: a
  /// thread that is still a member when the activity closes loses its data.
  class Guard {
  public:
    Guard(Guard &&other)
        : m_scope(std::exchange(other.m_scope, nullptr)),
          m_record(std::move(other.m_record)) {}
    Guard &operator=(Guard &&) = delete;
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

    ~Guard() {
      if (m_scope)
        m_scope->Remove(m_record);
    }

    const DataSP &GetData() const { return m_record->m_data; }

    /// A token for another thread to join this activity.
    Token Share() const { return Token(*m_scope, m_record); }

  private:
    friend class SharedScope;
    Guard(SharedScope &scope, std::shared_ptr<Record> record)
        : m_scope(&scope), m_record(std::move(record)) {}

    SharedScope *m_scope = nullptr;
    std::shared_ptr<Record> m_record;
  };

  /// Open an activity owned by the calling thread, whose members use \p data.
  [[nodiscard]] Guard Enter(DataSP data) {
    auto record =
        std::make_shared<Record>(std::this_thread::get_id(), std::move(data));
    std::lock_guard<std::mutex> guard(m_mutex);
    m_records.push_back(record);
    m_num_records.store(m_records.size(), std::memory_order_release);
    return Guard(*this, std::move(record));
  }

  /// Return the data of the innermost open activity the calling thread is a
  /// member of, or null if it isn't a member of any.
  DataSP GetDataForCurrentThread() const {
    if (m_num_records.load(std::memory_order_acquire) == 0)
      return nullptr;
    const std::thread::id self = std::this_thread::get_id();
    std::lock_guard<std::mutex> guard(m_mutex);
    for (const std::shared_ptr<Record> &record : llvm::reverse(m_records))
      if (llvm::is_contained(record->m_members, self))
        return record->m_data;
    return nullptr;
  }

  /// Return true if an activity is open that the calling thread isn't a
  /// member of.
  bool IsActiveOnOtherThread() const {
    if (m_num_records.load(std::memory_order_acquire) == 0)
      return false;
    const std::thread::id self = std::this_thread::get_id();
    std::lock_guard<std::mutex> guard(m_mutex);
    return llvm::any_of(m_records, [self](const std::shared_ptr<Record> &r) {
      return !llvm::is_contained(r->m_members, self);
    });
  }

private:
  void Remove(const std::shared_ptr<Record> &record) {
    std::lock_guard<std::mutex> guard(m_mutex);
    llvm::erase(m_records, record);
    m_num_records.store(m_records.size(), std::memory_order_release);
  }

  /// Add \p thread to \p record, if the activity is still open.
  bool Join(const std::shared_ptr<Record> &record, std::thread::id thread) {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!llvm::is_contained(m_records, record))
      return false;
    record->m_members.push_back(thread);
    return true;
  }

  void Leave(Record &record, std::thread::id thread) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = llvm::find(record.m_members, thread);
    if (it != record.m_members.end())
      record.m_members.erase(it);
  }

  mutable std::mutex m_mutex;
  std::vector<std::shared_ptr<Record>> m_records;
  /// m_records.size(), readable without the mutex, so that threads can tell
  /// cheaply that nothing is open.
  std::atomic<size_t> m_num_records{0};
};

} // namespace lldb_private

#endif // LLDB_UTILITY_SHAREDSCOPE_H
