//===-- IOHandlerPicker.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/IOHandlerPicker.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Host/File.h"
#include "lldb/Host/StreamFile.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"

#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <numeric>

#define ESCAPE "\x1b"
#define ANSI_NORMAL ESCAPE "[0m"
#define ANSI_BOLD ESCAPE "[1m"
#define ANSI_FAINT ESCAPE "[2m"
#define ANSI_UNDERLINE ESCAPE "[4m"
#define ANSI_REVERSE_VIDEO ESCAPE "[7m"
#define ANSI_CURSOR_HOME ESCAPE "[H"
#define ANSI_HIDE_CURSOR ESCAPE "[?25l"
#define ANSI_SHOW_CURSOR ESCAPE "[?25h"
#define ANSI_ALT_SCREEN_ON ESCAPE "[?1049h"
#define ANSI_ALT_SCREEN_OFF ESCAPE "[?1049l"

using namespace lldb;
using namespace lldb_private;

IOHandlerPicker::IOHandlerPicker(Debugger &debugger, llvm::StringRef header,
                                 std::vector<PickerItem> items,
                                 PickerMode mode)
    : IOHandler(debugger, IOHandler::Type::Picker), m_header(header.str()),
      m_items(std::move(items)), m_mode(mode) {
  m_checked.resize(m_items.size(), false);
  for (size_t i = 0; i < m_items.size(); ++i) {
    if (m_items[i].initially_selected)
      m_checked[i] = true;
  }

  m_sorted_indices.resize(m_items.size());
  std::iota(m_sorted_indices.begin(), m_sorted_indices.end(), 0);
}

IOHandlerPicker::~IOHandlerPicker() = default;

void IOHandlerPicker::ComputeColumnWidths() {
  if (m_items.empty())
    return;

  size_t num_cols = m_items[0].columns.size();
  m_column_widths.assign(num_cols, 0);

  // First pass: compute minimum width per column (max of header/values).
  for (size_t c = 0; c < num_cols; ++c) {
    m_column_widths[c] = m_items[0].columns[c].name.size() + 2; // +2 for sort indicator
    for (const auto &item : m_items) {
      if (c < item.columns.size())
        m_column_widths[c] =
            std::max(m_column_widths[c], item.columns[c].value.size());
    }
  }

  // Second pass: distribute remaining terminal width across columns.
  // Account for prefix (" > " or " [x] ") and 2-char gaps between columns.
  size_t prefix_width = (m_mode == PickerMode::MultiSelect) ? 6 : 3;
  size_t used = prefix_width;
  for (size_t c = 0; c < num_cols; ++c)
    used += m_column_widths[c] + 2; // +2 for gap

  if (m_terminal_width > used) {
    size_t extra = m_terminal_width - used;
    size_t per_col = extra / num_cols;
    size_t remainder = extra % num_cols;
    for (size_t c = 0; c < num_cols; ++c)
      m_column_widths[c] += per_col + (c < remainder ? 1 : 0);
  }
}

size_t IOHandlerPicker::GetViewportHeight() const {
  // Reserve lines for: header, instructions, column headers, footer, margin.
  if (m_terminal_height <= 5)
    return 1;
  return m_terminal_height - 5;
}

bool IOHandlerPicker::EnterRawMode() {
#if LLDB_ENABLE_TERMIOS
  int fd = GetInputFD();
  if (fd < 0)
    return false;

  Terminal terminal(fd);
  m_saved_terminal_state.Save(terminal, false);
  if (auto err = terminal.SetRaw()) {
    llvm::consumeError(std::move(err));
    m_saved_terminal_state.Clear();
    return false;
  }
  return true;
#else
  return false;
#endif
}

void IOHandlerPicker::RestoreTerminalMode() {
#if LLDB_ENABLE_TERMIOS
  if (m_saved_terminal_state.IsValid()) {
    m_saved_terminal_state.Restore();
    m_saved_terminal_state.Clear();
  }
#endif
}

IOHandlerPicker::Key IOHandlerPicker::ReadKey() {
  int fd = GetInputFD();
  if (fd < 0)
    return Key::Escape;

  char c = 0;
  ssize_t n = ::read(fd, &c, 1);
  if (n <= 0)
    return Key::Escape;

  if (c == '\x1b') {
    // Check if more bytes follow (escape sequence) or bare ESC.
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms

    if (select(fd + 1, &read_fds, nullptr, nullptr, &tv) > 0) {
      char seq[4] = {};
      n = ::read(fd, &seq[0], 1);
      if (n <= 0)
        return Key::Escape;

      if (seq[0] == '[') {
        n = ::read(fd, &seq[1], 1);
        if (n <= 0)
          return Key::Escape;

        switch (seq[1]) {
        case 'A':
          return Key::Up;
        case 'B':
          return Key::Down;
        case 'C':
          return Key::Right;
        case 'D':
          return Key::Left;
        case 'H':
          return Key::Home;
        case 'F':
          return Key::End;
        case '5':
          // PgUp: \x1b[5~
          ::read(fd, &seq[2], 1); // consume '~'
          return Key::PageUp;
        case '6':
          // PgDn: \x1b[6~
          ::read(fd, &seq[2], 1); // consume '~'
          return Key::PageDown;
        default:
          return Key::Unknown;
        }
      } else if (seq[0] == 'O') {
        // vt100 application mode: \x1bOA, \x1bOB, etc.
        n = ::read(fd, &seq[1], 1);
        if (n <= 0)
          return Key::Escape;

        switch (seq[1]) {
        case 'A':
          return Key::Up;
        case 'B':
          return Key::Down;
        case 'C':
          return Key::Right;
        case 'D':
          return Key::Left;
        case 'H':
          return Key::Home;
        case 'F':
          return Key::End;
        default:
          return Key::Unknown;
        }
      }
      return Key::Unknown;
    }
    return Key::Escape;
  }

  switch (c) {
  case '\r':
  case '\n':
    return Key::Enter;
  case ' ':
    return Key::Space;
  case 'j':
    return Key::Char_j;
  case 'k':
    return Key::Char_k;
  case 'h':
    return Key::Char_h;
  case 'l':
    return Key::Char_l;
  case 's':
    return Key::Char_s;
  case 'S':
    return Key::Char_S;
  case 3: // Ctrl+C
    return Key::Escape;
  default:
    return Key::Unknown;
  }
}

void IOHandlerPicker::ScrollToCursor() {
  size_t viewport = GetViewportHeight();

  if (m_cursor_index < m_viewport_start)
    m_viewport_start = m_cursor_index;
  else if (m_cursor_index >= m_viewport_start + viewport)
    m_viewport_start = m_cursor_index - viewport + 1;
}

void IOHandlerPicker::SortByColumn(std::optional<size_t> col_index,
                                   bool ascending) {
  // Remember which original item the cursor is on.
  size_t cursor_item =
      m_cursor_index < m_sorted_indices.size()
          ? m_sorted_indices[m_cursor_index]
          : 0;

  m_sort_column_index = col_index;
  m_sort_ascending = ascending;

  if (!col_index) {
    // Reset to original order.
    std::iota(m_sorted_indices.begin(), m_sorted_indices.end(), 0);
  } else {
    // Always start from natural order so stable_sort is deterministic.
    std::iota(m_sorted_indices.begin(), m_sorted_indices.end(), 0);

    size_t col = *col_index;
    bool numeric = false;
    if (!m_items.empty() && col < m_items[0].columns.size())
      numeric = m_items[0].columns[col].is_numeric;

    std::stable_sort(m_sorted_indices.begin(), m_sorted_indices.end(),
                     [this, col, numeric, ascending](size_t a, size_t b) {
                       const std::string &va = m_items[a].columns[col].value;
                       const std::string &vb = m_items[b].columns[col].value;

                       int cmp;
                       if (numeric) {
                         long long na = 0, nb = 0;
                         llvm::StringRef(va).getAsInteger(10, na);
                         llvm::StringRef(vb).getAsInteger(10, nb);
                         if (na < nb)
                           cmp = -1;
                         else if (na > nb)
                           cmp = 1;
                         else
                           cmp = 0;
                       } else {
                         cmp = va.compare(vb);
                       }

                       return ascending ? (cmp < 0) : (cmp > 0);
                     });
  }

  // Reset cursor to top after re-sort so the new order is visible.
  m_cursor_index = 0;
  m_viewport_start = 0;
}

void IOHandlerPicker::Render() {
  int fd = GetOutputFD();
  if (fd < 0)
    return;

  int width = static_cast<int>(m_terminal_width);

  // Build the entire frame into a single buffer.
  std::string frame;

  // Move cursor to top-left of the alternate screen.
  frame += ANSI_CURSOR_HOME;

  // Helper: pad visible content to terminal width.
  auto pad = [&](const std::string &s, size_t visible_len) -> std::string {
    if (visible_len < static_cast<size_t>(width))
      return s + std::string(width - visible_len, ' ');
    return s;
  };

  // Helper: append one full line to the frame buffer.
  auto write_line = [&](const std::string &content) {
    frame += "\r";
    frame += content;
    frame += "\r\n";
  };

  // --- Header line (bold) ---
  write_line(std::string(ANSI_BOLD) + pad(m_header, m_header.size()) +
             ANSI_NORMAL);

  // --- Instructions line (faint) ---
  {
    std::string text = "  ";
    if (m_mode == PickerMode::SingleSelect)
      text += "jk: navigate  Enter: select  s: sort  ESC: cancel";
    else
      text +=
          "jk: navigate  Space: toggle  Enter: confirm  s: sort  ESC: cancel";
    write_line(std::string(ANSI_FAINT) + pad(text, text.size()) + ANSI_NORMAL);
  }

  // --- Column header row ---
  {
    std::string line;
    size_t visible_len = 0;

    line += ANSI_FAINT;

    std::string prefix = "  ";
    if (m_mode == PickerMode::MultiSelect)
      prefix += "     ";
    line += prefix;
    visible_len += prefix.size();

    size_t num_cols = m_items.empty() ? 0 : m_items[0].columns.size();
    for (size_t c = 0; c < num_cols; ++c) {
      const auto &col_name = m_items[0].columns[c].name;
      bool is_sort_col = m_sort_column_index && *m_sort_column_index == c;

      if (is_sort_col)
        line += std::string(ANSI_NORMAL) + ANSI_BOLD + ANSI_UNDERLINE;

      std::string cell = col_name;
      if (cell.size() < m_column_widths[c])
        cell.append(m_column_widths[c] - cell.size(), ' ');
      line += cell;
      visible_len += m_column_widths[c];

      if (is_sort_col) {
        line += m_sort_ascending ? " ^" : " v";
        visible_len += 2;
        line += std::string(ANSI_NORMAL) + ANSI_FAINT;
      }
      line += "  ";
      visible_len += 2;
    }

    if (visible_len < static_cast<size_t>(width))
      line.append(width - visible_len, ' ');
    line += ANSI_NORMAL;
    write_line(line);
  }

  // --- Items ---
  size_t viewport = GetViewportHeight();
  size_t max_visible = std::min(viewport, m_sorted_indices.size());

  for (size_t vi = 0; vi < max_visible; ++vi) {
    size_t display_idx = m_viewport_start + vi;
    if (display_idx < m_sorted_indices.size()) {
      size_t item_idx = m_sorted_indices[display_idx];
      const auto &item = m_items[item_idx];
      bool is_cursor = (display_idx == m_cursor_index);

      std::string line;
      size_t visible_len = 0;

      if (is_cursor)
        line += ANSI_REVERSE_VIDEO;

      if (m_mode == PickerMode::MultiSelect) {
        char buf[8];
        snprintf(buf, sizeof(buf), " [%c] ", m_checked[item_idx] ? 'x' : ' ');
        line += buf;
        visible_len += 6;
      } else {
        char buf[8];
        snprintf(buf, sizeof(buf), " %c ", is_cursor ? '>' : ' ');
        line += buf;
        visible_len += 3;
      }

      for (size_t c = 0; c < item.columns.size(); ++c) {
        std::string cell = item.columns[c].value;
        if (cell.size() < m_column_widths[c])
          cell.append(m_column_widths[c] - cell.size(), ' ');
        line += cell + "  ";
        visible_len += m_column_widths[c] + 2;
      }

      if (visible_len < static_cast<size_t>(width))
        line.append(width - visible_len, ' ');

      if (is_cursor)
        line += ANSI_NORMAL;

      write_line(line);
    }
  }

  // --- Footer line ---
  {
    std::string text = "  ";
    if (m_mode == PickerMode::MultiSelect) {
      size_t selected = 0;
      for (bool c : m_checked)
        if (c)
          selected++;
      char buf[64];
      snprintf(buf, sizeof(buf), "%zu/%zu selected", selected, m_items.size());
      text += buf;
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%zu/%zu", m_cursor_index + 1,
               m_sorted_indices.size());
      text += buf;
    }

    if (m_viewport_start + max_visible < m_sorted_indices.size())
      text += "  [more below]";
    if (m_viewport_start > 0)
      text += "  [more above]";

    write_line(std::string(ANSI_FAINT) + pad(text, text.size()) + ANSI_NORMAL);
  }

  // Write the entire frame in a single write() syscall.
  ::write(fd, frame.data(), frame.size());
}

void IOHandlerPicker::Run() {
  if (!GetIsRealTerminal() || m_items.empty()) {
    m_result.was_canceled = true;
    SetIsDone(true);
    return;
  }

  m_terminal_width = m_debugger.GetTerminalWidth();
  m_terminal_height = m_debugger.GetTerminalHeight();

  ComputeColumnWidths();

  if (!EnterRawMode()) {
    m_result.was_canceled = true;
    SetIsDone(true);
    return;
  }

  // Switch to alternate screen buffer and hide cursor.
  int out_fd = GetOutputFD();
  if (out_fd >= 0) {
    const char *init = ANSI_ALT_SCREEN_ON ANSI_HIDE_CURSOR;
    ::write(out_fd, init, strlen(init));
  }

  size_t num_cols =
      m_items.empty() ? 0 : m_items[0].columns.size();

  ScrollToCursor();
  Render();

  while (!GetIsDone()) {
    Key key = ReadKey();

    LLDB_LOG(GetLog(LLDBLog::Host), "IOHandlerPicker: key={0}",
             static_cast<int>(key));

    switch (key) {
    case Key::Up:
    case Key::Char_k:
      if (m_cursor_index > 0)
        m_cursor_index--;
      break;

    case Key::Down:
    case Key::Char_j:
      if (m_cursor_index + 1 < m_sorted_indices.size())
        m_cursor_index++;
      break;

    case Key::PageUp: {
      size_t page = GetViewportHeight();
      m_cursor_index =
          m_cursor_index > page ? m_cursor_index - page : 0;
      break;
    }

    case Key::PageDown: {
      size_t page = GetViewportHeight();
      m_cursor_index = std::min(m_cursor_index + page,
                                m_sorted_indices.size() - 1);
      break;
    }

    case Key::Home:
      m_cursor_index = 0;
      break;

    case Key::End:
      if (!m_sorted_indices.empty())
        m_cursor_index = m_sorted_indices.size() - 1;
      break;

    case Key::Left:
    case Key::Right:
    case Key::Char_h:
    case Key::Char_l:
      // No action for left/right in picker.
      break;

    case Key::Char_s:
    case Key::Char_S:
      // Cycle sort: unsorted → col0 asc → col0 desc → col1 asc → ... → unsorted
      if (num_cols > 0) {
        if (!m_sort_column_index) {
          // unsorted → first column ascending
          SortByColumn(0, true);
        } else if (m_sort_ascending) {
          // ascending → descending (same column)
          SortByColumn(m_sort_column_index, false);
        } else if (*m_sort_column_index + 1 < num_cols) {
          // descending → next column ascending
          SortByColumn(*m_sort_column_index + 1, true);
        } else {
          // last column descending → unsorted
          SortByColumn(std::nullopt, true);
        }
      }
      break;

    case Key::Space:
      if (m_mode == PickerMode::MultiSelect &&
          m_cursor_index < m_sorted_indices.size()) {
        size_t item_idx = m_sorted_indices[m_cursor_index];
        m_checked[item_idx] = !m_checked[item_idx];
      }
      break;

    case Key::Enter:
      if (m_mode == PickerMode::SingleSelect) {
        if (m_cursor_index < m_sorted_indices.size()) {
          size_t item_idx = m_sorted_indices[m_cursor_index];
          m_result.selected_ids.push_back(m_items[item_idx].id);
        }
      } else {
        for (size_t i = 0; i < m_items.size(); ++i) {
          if (m_checked[i])
            m_result.selected_ids.push_back(m_items[i].id);
        }
      }
      m_result.was_canceled = false;
      SetIsDone(true);
      break;

    case Key::Escape:
      m_result.was_canceled = true;
      SetIsDone(true);
      break;

    case Key::Unknown:
      break;
    }

    if (!GetIsDone()) {
      ScrollToCursor();
      Render();
    }
  }

  // Leave alternate screen buffer (restores original screen) and show cursor.
  if (out_fd >= 0) {
    const char *cleanup = ANSI_SHOW_CURSOR ANSI_ALT_SCREEN_OFF;
    ::write(out_fd, cleanup, strlen(cleanup));
  }
  RestoreTerminalMode();
}

void IOHandlerPicker::Cancel() {
  // Only mark as canceled if Run() hasn't already committed a result.
  if (!GetIsDone())
    m_result.was_canceled = true;
  SetIsDone(true);
}

bool IOHandlerPicker::Interrupt() {
  if (!GetIsDone())
    m_result.was_canceled = true;
  SetIsDone(true);
  return true;
}

void IOHandlerPicker::GotEOF() {
  if (!GetIsDone())
    m_result.was_canceled = true;
  SetIsDone(true);
}
