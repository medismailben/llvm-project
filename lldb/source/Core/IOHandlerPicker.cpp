//===-- IOHandlerPicker.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/IOHandlerPicker.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Host/Config.h"
#include "lldb/Host/StreamFile.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include "lldb/Utility/SelectHelper.h"

#if LLDB_ENABLE_TERMIOS
#include <unistd.h>
#endif

#include <algorithm>
#include <cassert>

#define ESCAPE "\x1b"
#define ANSI_NORMAL ESCAPE "[0m"
#define ANSI_BOLD ESCAPE "[1m"
#define ANSI_FAINT ESCAPE "[2m"
#define ANSI_UNDERLINE ESCAPE "[4m"
#define ANSI_REVERSE_VIDEO ESCAPE "[7m"
#define ANSI_CURSOR_HOME ESCAPE "[H"
#define ANSI_CLEAR_SCREEN ESCAPE "[2J"
#define ANSI_HIDE_CURSOR ESCAPE "[?25l"
#define ANSI_SHOW_CURSOR ESCAPE "[?25h"
#define ANSI_DISABLE_AUTO_WRAP ESCAPE "[?7l"
#define ANSI_ENABLE_AUTO_WRAP ESCAPE "[?7h"
#define ANSI_ALT_SCREEN_ON ESCAPE "[?1049h"
#define ANSI_ALT_SCREEN_OFF ESCAPE "[?1049l"

using namespace lldb;
using namespace lldb_private;

static constexpr size_t kColumnGap = 2;

IOHandlerPicker::IOHandlerPicker(Debugger &debugger, llvm::StringRef header,
                                 std::vector<PickerItem> items,
                                 PickerMode mode)
    : IOHandler(debugger, IOHandler::Type::Picker), m_header(header.str()),
      m_items(std::move(items)), m_mode(mode) {
  assert(llvm::all_of(m_items,
                      [&](const PickerItem &item) {
                        return item.columns.size() ==
                               m_items.front().columns.size();
                      }) &&
         "all picker items must have the same columns");

  for (const PickerItem &item : m_items)
    m_checked.push_back(item.initially_selected);
}

IOHandlerPicker::~IOHandlerPicker() = default;

size_t IOHandlerPicker::GetPrefixWidth() const {
  // " [x] " in multi-select mode, " > " in single-select mode.
  return m_mode == PickerMode::MultiSelect ? 5 : 3;
}

void IOHandlerPicker::ComputeColumnWidths() {
  const std::vector<PickerColumn> &first = m_items.front().columns;
  m_column_widths.clear();
  for (size_t c = 0; c < first.size(); ++c) {
    size_t width = first[c].name.size();
    for (const PickerItem &item : m_items)
      width = std::max(width, item.columns[c].value.size());
    m_column_widths.push_back(width);
  }

  m_row_width = GetPrefixWidth();
  for (size_t width : m_column_widths)
    m_row_width += width + kColumnGap;

  // Spread the leftover terminal width across the columns.
  size_t num_cols = m_column_widths.size();
  if (num_cols && m_row_width < m_terminal_width) {
    size_t extra = m_terminal_width - m_row_width;
    for (size_t c = 0; c < num_cols; ++c)
      m_column_widths[c] += extra / num_cols + (c < extra % num_cols ? 1 : 0);
    m_row_width = m_terminal_width;
  }
}

void IOHandlerPicker::UpdateLayout() {
  m_terminal_width = m_debugger.GetTerminalWidth();
  m_terminal_height = m_debugger.GetTerminalHeight();
  ComputeColumnWidths();
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
  if (m_saved_terminal_state.IsValid()) {
    m_saved_terminal_state.Restore();
    m_saved_terminal_state.Clear();
  }
}

IOHandlerPicker::Key IOHandlerPicker::ReadKey() {
#if LLDB_ENABLE_TERMIOS
  int fd = GetInputFD();
  auto read_byte = [fd]() -> std::optional<char> {
    char c;
    if (::read(fd, &c, 1) == 1)
      return c;
    return std::nullopt;
  };

  // Block until a key arrives or TerminalSizeChanged() wakes us up.
  int resize_fd = m_resize_pipe.GetReadFileDescriptor();
  SelectHelper wait_for_input;
  wait_for_input.FDSetRead(fd);
  if (m_resize_pipe.CanRead())
    wait_for_input.FDSetRead(resize_fd);
  if (wait_for_input.Select().Fail())
    return Key::Escape;
  if (m_resize_pipe.CanRead() && wait_for_input.FDIsSetRead(resize_fd)) {
    char buf[32];
    if (llvm::Expected<size_t> bytes = m_resize_pipe.Read(buf, sizeof(buf));
        !bytes)
      llvm::consumeError(bytes.takeError());
    return Key::Resize;
  }

  std::optional<char> c = read_byte();
  if (!c)
    return Key::Escape;

  switch (*c) {
  case '\r':
  case '\n':
    return Key::Enter;
  case ' ':
    return Key::Space;
  case 'k':
    return Key::Up;
  case 'j':
    return Key::Down;
  case 3: // Raw mode disables SIGINT, so Ctrl+C arrives as a byte.
    return Key::Escape;
  case '\x1b':
    break;
  default:
    return Key::Unknown;
  }

  // A bare ESC looks like the start of an escape sequence, so only treat it as
  // one if more bytes follow shortly.
  SelectHelper wait_for_sequence;
  wait_for_sequence.SetTimeout(std::chrono::milliseconds(100));
  wait_for_sequence.FDSetRead(fd);
  if (wait_for_sequence.Select().Fail())
    return Key::Escape;

  // Accept both CSI ("ESC [") and vt100 application mode ("ESC O") sequences.
  std::optional<char> intro = read_byte();
  if (intro != '[' && intro != 'O')
    return Key::Unknown;

  std::optional<char> code = read_byte();
  if (!code)
    return Key::Escape;

  switch (*code) {
  case 'A':
    return Key::Up;
  case 'B':
    return Key::Down;
  case 'H':
    return Key::Home;
  case 'F':
    return Key::End;
  case '5':
  case '6':
    read_byte(); // Consume the trailing '~'.
    return *code == '5' ? Key::PageUp : Key::PageDown;
  default:
    return Key::Unknown;
  }
#else
  return Key::Escape;
#endif
}

void IOHandlerPicker::ScrollToCursor() {
  size_t viewport = GetViewportHeight();

  if (m_cursor_index < m_viewport_start)
    m_viewport_start = m_cursor_index;
  else if (m_cursor_index >= m_viewport_start + viewport)
    m_viewport_start = m_cursor_index - viewport + 1;
}

void IOHandlerPicker::Render() {
  // Build the whole frame first so it reaches the terminal in one write.
  std::string frame = ANSI_CURSOR_HOME;

  // Pad every line to the terminal width so it overwrites the previous frame.
  auto write_line = [&](llvm::StringRef style, llvm::StringRef content,
                        size_t visible_len) {
    frame += style;
    frame += content;
    if (visible_len < m_terminal_width)
      frame.append(m_terminal_width - visible_len, ' ');
    frame += ANSI_NORMAL "\r\n";
  };

  write_line(ANSI_BOLD, m_header, m_header.size());

  llvm::StringRef help =
      m_mode == PickerMode::SingleSelect
          ? "  jk: navigate  Enter: select  ESC: cancel"
          : "  jk: navigate  Space: toggle  Enter: confirm  ESC: cancel";
  write_line(ANSI_FAINT, help, help.size());

  std::string column_names(GetPrefixWidth(), ' ');
  for (size_t c = 0; c < m_column_widths.size(); ++c) {
    const std::string &name = m_items.front().columns[c].name;
    column_names += name;
    column_names.append(m_column_widths[c] - name.size() + kColumnGap, ' ');
  }
  write_line(ANSI_FAINT, column_names, m_row_width);

  size_t end =
      std::min(m_viewport_start + GetViewportHeight(), m_items.size());
  for (size_t item_idx = m_viewport_start; item_idx < end; ++item_idx) {
    bool is_cursor = item_idx == m_cursor_index;

    std::string row;
    if (m_mode == PickerMode::MultiSelect)
      row = m_checked[item_idx] ? " [x] " : " [ ] ";
    else
      row = is_cursor ? " > " : "   ";

    for (size_t c = 0; c < m_column_widths.size(); ++c) {
      const std::string &value = m_items[item_idx].columns[c].value;
      row += value;
      row.append(m_column_widths[c] - value.size() + kColumnGap, ' ');
    }
    write_line(is_cursor ? ANSI_REVERSE_VIDEO : "", row, row.size());
  }

  std::string footer =
      m_mode == PickerMode::MultiSelect
          ? llvm::formatv("  {0}/{1} selected", llvm::count(m_checked, true),
                          m_items.size())
                .str()
          : llvm::formatv("  {0}/{1}", m_cursor_index + 1, m_items.size())
                .str();
  if (end < m_items.size())
    footer += "  [more below]";
  if (m_viewport_start > 0)
    footer += "  [more above]";
  write_line(ANSI_FAINT, footer, footer.size());

  GetOutputStreamFileSP()->Lock() << frame;
}

void IOHandlerPicker::Run() {
  if (!GetIsRealTerminal() || m_items.empty() || !EnterRawMode()) {
    m_result.was_canceled = true;
    SetIsDone(true);
    return;
  }

  // Without the pipe we can't notice resizes, but the picker still works.
  if (Status error = m_resize_pipe.CreateNew(); error.Fail())
    LLDB_LOG(GetLog(LLDBLog::Host), "IOHandlerPicker: {0}", error);

  UpdateLayout();

  // Hide the statusline before switching screens: its scroll region also
  // applies to the alternate screen, and its cursor save/restore shares the
  // slot that leaving the alternate screen restores.
  m_debugger.HideStatusline();

  // The alternate screen preserves the user's scrollback, and disabling
  // auto-wrap clips rows wider than the terminal instead of breaking the
  // layout.
  GetOutputStreamFileSP()->Lock()
      << ANSI_ALT_SCREEN_ON ANSI_HIDE_CURSOR ANSI_DISABLE_AUTO_WRAP;
  Render();

  while (!GetIsDone()) {
    switch (ReadKey()) {
    case Key::Up:
      if (m_cursor_index > 0)
        --m_cursor_index;
      break;
    case Key::Down:
      if (m_cursor_index + 1 < m_items.size())
        ++m_cursor_index;
      break;
    case Key::PageUp:
      m_cursor_index -= std::min(m_cursor_index, GetViewportHeight());
      break;
    case Key::PageDown:
      m_cursor_index =
          std::min(m_cursor_index + GetViewportHeight(), m_items.size() - 1);
      break;
    case Key::Home:
      m_cursor_index = 0;
      break;
    case Key::End:
      m_cursor_index = m_items.size() - 1;
      break;
    case Key::Resize:
      UpdateLayout();
      // Rows may now be narrower than the previous frame, so wipe it.
      GetOutputStreamFileSP()->Lock() << ANSI_CLEAR_SCREEN;
      break;
    case Key::Space:
      if (m_mode == PickerMode::MultiSelect)
        m_checked[m_cursor_index].flip();
      break;
    case Key::Enter:
      if (m_mode == PickerMode::SingleSelect) {
        m_result.selected_ids.push_back(m_items[m_cursor_index].id);
      } else {
        for (size_t i = 0; i < m_items.size(); ++i)
          if (m_checked[i])
            m_result.selected_ids.push_back(m_items[i].id);
      }
      SetIsDone(true);
      break;
    case Key::Escape:
      Cancel();
      break;
    case Key::Unknown:
      break;
    }

    if (!GetIsDone()) {
      ScrollToCursor();
      Render();
    }
  }

  GetOutputStreamFileSP()->Lock()
      << ANSI_ENABLE_AUTO_WRAP ANSI_SHOW_CURSOR ANSI_ALT_SCREEN_OFF;
  RestoreTerminalMode();
  m_resize_pipe.Close();
  m_debugger.ShowStatusline();
}

void IOHandlerPicker::Cancel() {
  // Don't clobber a result that Run() already committed.
  if (!GetIsDone())
    m_result.was_canceled = true;
  SetIsDone(true);
}

bool IOHandlerPicker::Interrupt() {
  Cancel();
  return true;
}

void IOHandlerPicker::GotEOF() { Cancel(); }

void IOHandlerPicker::TerminalSizeChanged() {
  if (!m_resize_pipe.CanWrite())
    return;
  char c = 0;
  if (llvm::Expected<size_t> bytes = m_resize_pipe.Write(&c, 1); !bytes)
    llvm::consumeError(bytes.takeError());
}
