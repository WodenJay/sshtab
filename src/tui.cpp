#include "tui.h"

#include "util.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

bool WriteAll(int fd, const std::string& data) {
  const char* buf = data.data();
  size_t left = data.size();
  while (left > 0) {
    ssize_t n = write(fd, buf, left);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    buf += n;
    left -= static_cast<size_t>(n);
  }
  return true;
}

bool ReadByte(int fd, char* out) {
  while (true) {
    ssize_t n = read(fd, out, 1);
    if (n == 1) {
      return true;
    }
    if (n == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    return false;
  }
}

bool HasControlChars(const std::string& s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) {
      return true;
    }
  }
  return false;
}

struct TerminalUiGuard {
  int fd = -1;
  termios orig{};
  bool raw_active = false;
  bool screen_active = false;

  bool EnableRaw(std::string* err) {
    if (tcgetattr(fd, &orig) != 0) {
      if (err) {
        *err = std::string("tcgetattr failed: ") + std::strerror(errno);
      }
      return false;
    }
    termios raw = orig;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= (CS8);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSAFLUSH, &raw) != 0) {
      if (err) {
        *err = std::string("tcsetattr failed: ") + std::strerror(errno);
      }
      return false;
    }
    raw_active = true;
    return true;
  }

  bool EnterScreen(std::string* err) {
    std::string seq;
    seq += "\x1b[?1049h";
    seq += "\x1b[H\x1b[2J";
    seq += "\x1b[?25l";
    if (!WriteAll(fd, seq)) {
      if (err) {
        *err = std::string("tty write failed: ") + std::strerror(errno);
      }
      return false;
    }
    screen_active = true;
    return true;
  }

  ~TerminalUiGuard() {
    if (raw_active) {
      tcsetattr(fd, TCSAFLUSH, &orig);
    }
    if (screen_active) {
      std::string seq;
      seq += "\x1b[?25h";
      seq += "\x1b[0m";
      seq += "\x1b[?1049l";
      WriteAll(fd, seq);
    }
  }
};

struct FdGuard {
  int fd = -1;
  ~FdGuard() {
    if (fd >= 0) {
      close(fd);
    }
  }
};

struct TerminalSize {
  size_t rows = 24;
  size_t cols = 80;
};

TerminalSize GetTerminalSize(int fd) {
  struct winsize ws;
  if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
    TerminalSize size;
    size.rows = static_cast<size_t>(ws.ws_row);
    size.cols = static_cast<size_t>(ws.ws_col);
    return size;
  }
  return TerminalSize{};
}

size_t GetVisibleCount(size_t total, size_t rows) {
  size_t max_visible = 10;
  if (rows > 4) {
    max_visible = rows - 4;
  } else if (rows > 0) {
    max_visible = 1;
  }
  return total < max_visible ? total : max_visible;
}

size_t GetPadding(size_t width) {
  if (width >= 4) {
    return 2;
  }
  if (width >= 2) {
    return 1;
  }
  return 0;
}

std::string TruncateLine(const std::string& text, size_t width) {
  if (width == 0) {
    return std::string();
  }
  if (text.size() <= width) {
    return text;
  }
  if (width <= 3) {
    return text.substr(0, width);
  }
  return text.substr(0, width - 3) + "...";
}

std::string FormatRelativeTime(std::int64_t last_used, std::time_t now) {
  if (last_used <= 0 || now <= 0) {
    return "?";
  }
  long long diff = static_cast<long long>(now) - static_cast<long long>(last_used);
  if (diff < 0) {
    diff = 0;
  }
  if (diff < 60) {
    return "now";
  }
  if (diff < 3600) {
    return std::to_string(diff / 60) + "m";
  }
  if (diff < 86400) {
    return std::to_string(diff / 3600) + "h";
  }
  if (diff < 604800) {
    return std::to_string(diff / 86400) + "d";
  }

  std::time_t ts = static_cast<std::time_t>(last_used);
  std::tm tm{};
  if (!localtime_r(&ts, &tm)) {
    return "?";
  }
  char buf[16];
  if (std::strftime(buf, sizeof(buf), "%Y/%m/%d", &tm) == 0) {
    return "?";
  }
  return std::string(buf);
}

std::string TrimTitle(const std::string& title) {
  size_t end = title.find('(');
  std::string base = end == std::string::npos ? title : title.substr(0, end);
  size_t start = base.find_first_not_of(' ');
  if (start == std::string::npos) {
    return std::string();
  }
  size_t last = base.find_last_not_of(' ');
  return base.substr(start, last - start + 1);
}

std::string BuildMetaLine(const PickItem& item) {
  std::string out;
  if (!item.host.empty()) {
    out += "host: " + item.host;
  }
  if (!item.port.empty()) {
    if (!out.empty()) {
      out += "  ";
    }
    out += "p:" + item.port;
  }
  if (!item.jump.empty()) {
    if (!out.empty()) {
      out += "  ";
    }
    out += "J:" + item.jump;
  }
  if (!item.identity.empty()) {
    if (!out.empty()) {
      out += "  ";
    }
    out += "i:" + item.identity;
  }
  return out;
}

std::string PickItemLabel(const PickItem& item, bool show_alias) {
  if (show_alias && !item.alias.empty()) {
    return item.alias;
  }
  return item.display;
}

std::string BuildHintText(const PickUiConfig& config, bool show_alias, size_t selected, size_t total) {
  std::string hint = "Up/Down move  Enter confirm  Esc cancel";
  if (config.allow_alias_edit) {
    hint += "  n alias";
  }
  if (config.allow_display_toggle) {
    hint += "  Shift+Tab/S toggle";
    hint += show_alias ? "  view: alias" : "  view: addr";
  }
  hint += "  ";
  hint += std::to_string(selected + 1);
  hint += "/";
  hint += std::to_string(total);
  return hint;
}

void AppendStyledLine(std::string* out,
                      const std::string& text,
                      size_t width,
                      size_t padding,
                      const std::string& style) {
  if (!out) {
    return;
  }
  size_t inner_width = width > padding * 2 ? width - padding * 2 : 0;
  std::string line = TruncateLine(text, inner_width);
  out->append("\r\x1b[2K");
  out->append(style);
  out->append(padding, ' ');
  out->append(line);
  if (line.size() < inner_width) {
    out->append(inner_width - line.size(), ' ');
  }
  out->append(padding, ' ');
  out->append("\x1b[0m");
  out->append("\n");
}

void AppendListLine(std::string* out,
                    const std::string& left,
                    const std::string& right,
                    size_t width,
                    size_t padding,
                    const std::string& style) {
  if (!out) {
    return;
  }
  size_t inner_width = width > padding * 2 ? width - padding * 2 : 0;
  std::string right_text = right;
  size_t right_len = right_text.size();
  size_t gap = right_text.empty() ? 0 : 2;
  size_t left_max = inner_width;
  if (right_len + gap <= inner_width) {
    left_max = inner_width - right_len - gap;
  } else {
    right_text.clear();
    right_len = 0;
    gap = 0;
    left_max = inner_width;
  }
  std::string left_text = TruncateLine(left, left_max);
  out->append("\r\x1b[2K");
  out->append(style);
  out->append(padding, ' ');
  out->append(left_text);
  if (left_text.size() < left_max) {
    out->append(left_max - left_text.size(), ' ');
  }
  if (!right_text.empty()) {
    out->append(gap, ' ');
    out->append(right_text);
  }
  out->append(padding, ' ');
  out->append("\x1b[0m");
  out->append("\n");
}

bool Draw(int fd,
          const std::vector<PickItem>& items,
          const std::string& title,
          size_t selected,
          size_t offset,
          bool show_alias,
          const std::string& footer_left,
          const std::string& footer_right) {
  const TerminalSize size = GetTerminalSize(fd);
  const size_t width = size.cols;
  const size_t rows = size.rows;
  const size_t padding = GetPadding(width);
  const size_t visible = GetVisibleCount(items.size(), rows);
  const std::string header_bg = "\x1b[48;5;235m";
  const std::string panel_bg = "\x1b[48;5;236m";
  const std::string select_bg = "\x1b[48;5;24m";
  const std::string text = "\x1b[38;5;250m";
  const std::string muted = "\x1b[38;5;245m";
  const std::string accent = "\x1b[38;5;75m";
  const std::string bright = "\x1b[38;5;231m";
  const std::string bold = "\x1b[1m";
  std::string out;
  out += "\x1b[2J\x1b[H";
  std::string title_base = TrimTitle(title);
  if (title_base.empty()) {
    title_base = "sshtab";
  }
  std::string header_text = title_base + "  [" + std::to_string(items.size()) + "]";
  AppendStyledLine(&out, header_text, width, padding, header_bg + accent + bold);

  std::string rule(width > padding * 2 ? width - padding * 2 : 0, '-');
  AppendStyledLine(&out, rule, width, padding, header_bg + muted);

  std::time_t now = std::time(nullptr);
  size_t inner_width = width > padding * 2 ? width - padding * 2 : 0;
  for (size_t i = 0; i < visible; ++i) {
    size_t idx = offset + i;
    if (idx >= items.size()) {
      AppendStyledLine(&out, "", width, padding, panel_bg + muted);
      continue;
    }
    const PickItem& item = items[idx];
    std::string prefix = idx == selected ? "> " : "  ";
    std::string line = prefix + PickItemLabel(item, show_alias);
    std::string time_text = FormatRelativeTime(item.last_used, now);
    std::string right_text = time_text + "  " + std::to_string(item.count) + "x";
    size_t gap = 2;
    if (right_text.size() + gap > inner_width) {
      right_text = time_text;
    }
    if (!right_text.empty() && right_text.size() + gap > inner_width) {
      right_text.clear();
    }
    if (idx == selected) {
      AppendListLine(&out, line, right_text, width, padding, select_bg + bright + bold);
    } else {
      AppendListLine(&out, line, right_text, width, padding, panel_bg + text);
    }
  }

  AppendStyledLine(&out, rule, width, padding, header_bg + muted);
  AppendListLine(&out, footer_left, footer_right, width, padding, header_bg + muted);
  return WriteAll(fd, out);
}

}  // namespace

PickResult RunPickTui(std::vector<PickItem>& items,
                      const std::string& title,
                      std::size_t* index,
                      const PickUiConfig& config,
                      const AliasUpdateFn& alias_update,
                      std::string* err) {
  if (items.empty()) {
    return PickResult::kCanceled;
  }
  if (!index) {
    if (err) {
      *err = "index pointer is null";
    }
    return PickResult::kError;
  }

  int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    if (err) {
      *err = std::string("open /dev/tty failed: ") + std::strerror(errno);
    }
    return PickResult::kError;
  }

  FdGuard fd_guard;
  fd_guard.fd = fd;

  TerminalUiGuard term;
  term.fd = fd;
  if (!term.EnableRaw(err)) {
    return PickResult::kError;
  }
  if (!term.EnterScreen(err)) {
    return PickResult::kError;
  }

  size_t selected = 0;
  size_t offset = 0;
  bool show_alias = config.show_alias;
  bool prompt_active = false;
  std::string prompt_input;
  std::string status;
  bool clear_status_on_next_input = false;

  auto draw = [&]() -> bool {
    std::string footer_left;
    std::string footer_right;
    if (prompt_active) {
      footer_left = "alias: " + prompt_input;
      footer_right = "Enter save  Esc cancel";
    } else {
      if (!status.empty()) {
        footer_left = status;
      } else {
        footer_left = BuildMetaLine(items[selected]);
      }
      footer_right = BuildHintText(config, show_alias, selected, items.size());
    }
    return Draw(fd, items, title, selected, offset, show_alias, footer_left, footer_right);
  };

  if (!draw()) {
    return PickResult::kError;
  }

  while (true) {
    char c = 0;
    if (!ReadByte(fd, &c)) {
      continue;
    }

    if (prompt_active) {
      if (c == 0x03) {
        prompt_active = false;
        prompt_input.clear();
        draw();
        continue;
      }
      if (c == '\r' || c == '\n') {
        std::string alias = TrimSpace(prompt_input);
        std::string update_err;
        if (!alias_update) {
          status = "alias update unavailable";
        } else if (HasControlChars(alias)) {
          status = "alias rejected: control characters";
        } else if (alias_update(items[selected], alias, &update_err)) {
          items[selected].alias = alias;
          status = alias.empty() ? "alias cleared" : "alias saved";
        } else {
          status = update_err.empty() ? "alias failed" : update_err;
        }
        prompt_active = false;
        prompt_input.clear();
        clear_status_on_next_input = true;
        draw();
        continue;
      }
      if (c == 0x1b) {
        char next = 0;
        if (!ReadByte(fd, &next)) {
          prompt_active = false;
          prompt_input.clear();
          draw();
          continue;
        }
        if (next != '[') {
          prompt_active = false;
          prompt_input.clear();
          draw();
          continue;
        }
        char code = 0;
        if (!ReadByte(fd, &code)) {
          continue;
        }
        if (code == 'A' || code == 'B' || code == 'Z') {
          draw();
          continue;
        }
        continue;
      }
      if (c == 0x7f || c == 0x08) {
        if (!prompt_input.empty()) {
          prompt_input.pop_back();
        }
        draw();
        continue;
      }
      unsigned char uc = static_cast<unsigned char>(c);
      if (uc >= 0x20 && uc != 0x7f) {
        prompt_input.push_back(c);
        draw();
      }
      continue;
    }

    if (clear_status_on_next_input) {
      status.clear();
      clear_status_on_next_input = false;
    }

    if (c == 0x03) {
      return PickResult::kCanceled;
    }

    if (c == '\r' || c == '\n') {
      *index = selected;
      return PickResult::kSelected;
    }

    if ((c == 'n' || c == 'N') && config.allow_alias_edit && alias_update) {
      prompt_active = true;
      prompt_input = items[selected].alias;
      draw();
      continue;
    }

    if ((c == 'S') && config.allow_display_toggle) {
      show_alias = !show_alias;
      draw();
      continue;
    }

    if (c == 0x1b) {
      char next = 0;
      if (!ReadByte(fd, &next)) {
        return PickResult::kCanceled;
      }
      if (next != '[') {
        return PickResult::kCanceled;
      }
      char code = 0;
      if (!ReadByte(fd, &code)) {
        return PickResult::kCanceled;
      }
      if (code == 'A') {
        if (selected > 0) {
          --selected;
        }
      } else if (code == 'B') {
        if (selected + 1 < items.size()) {
          ++selected;
        }
      } else if (code == 'Z' && config.allow_display_toggle) {
        show_alias = !show_alias;
      } else {
        continue;
      }
    }

    size_t visible = GetVisibleCount(items.size(), GetTerminalSize(fd).rows);
    if (selected < offset) {
      offset = selected;
    } else if (selected >= offset + visible) {
      offset = selected - visible + 1;
    }

    draw();
  }
}
