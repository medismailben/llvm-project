//===-- IOHandlerPicker.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_CORE_IOHANDLERPICKER_H
#define LLDB_CORE_IOHANDLERPICKER_H

#include "lldb/Core/IOHandler.h"
#include "lldb/Host/Pipe.h"
#include "lldb/Host/Terminal.h"

#include <optional>
#include <string>
#include <vector>

namespace lldb_private {

/// A single column in a picker item.
struct PickerColumn {
  std::string name;
  std::string value;
  bool is_numeric = false;
};

/// A selectable item in the picker. All items must have the same columns.
struct PickerItem {
  std::vector<PickerColumn> columns;
  std::string id;
  bool initially_selected = false;
};

enum class PickerMode { SingleSelect, MultiSelect };

struct PickerResult {
  bool was_canceled = false;
  /// The ids of the selected items, in the order they were given to the
  /// picker.
  std::vector<std::string> selected_ids;
};

/// An interactive ANSI-based picker IOHandler.
///
/// Supports single-select and multi-select modes, column sorting,
/// vim-style navigation, and scrolling. Works in any terminal that
/// supports basic ANSI escape codes. Falls back to canceled state
/// if the terminal is not interactive.
class IOHandlerPicker : public IOHandler {
public:
  IOHandlerPicker(Debugger &debugger, llvm::StringRef header,
                  std::vector<PickerItem> items, PickerMode mode);

  ~IOHandlerPicker() override;

  void Run() override;
  void Cancel() override;
  bool Interrupt() override;
  void GotEOF() override;
  void TerminalSizeChanged() override;

  const PickerResult &GetResult() const { return m_result; }

private:
  enum class Key {
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
    Space,
    Enter,
    Sort,
    Resize,
    Escape,
    Unknown,
  };

  bool EnterRawMode();
  void RestoreTerminalMode();
  Key ReadKey();
  void Render();
  void ScrollToCursor();
  void CycleSort();
  void SortByColumn(std::optional<size_t> col_index, bool ascending);
  void ComputeColumnWidths();
  void UpdateLayout();
  size_t GetPrefixWidth() const;
  size_t GetViewportHeight() const;

  std::string m_header;
  std::vector<PickerItem> m_items;
  PickerMode m_mode;
  PickerResult m_result;

  size_t m_cursor_index = 0;
  std::vector<bool> m_checked;
  size_t m_viewport_start = 0;

  std::vector<size_t> m_sorted_indices;
  std::optional<size_t> m_sort_column_index;
  bool m_sort_ascending = true;

  std::vector<size_t> m_column_widths;
  size_t m_row_width = 0;

  size_t m_terminal_width = 80;
  size_t m_terminal_height = 24;

  TerminalState m_saved_terminal_state;

  /// Written to by TerminalSizeChanged() to wake up ReadKey(), which may run
  /// on a different thread.
  Pipe m_resize_pipe;
};

} // namespace lldb_private

#endif // LLDB_CORE_IOHANDLERPICKER_H
