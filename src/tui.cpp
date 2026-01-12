#include "tui.h"

#include <cerrno>
#include <cstring>
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

bool Draw(int fd,
          const std::vector<PickItem>& items,
          const std::string& title,
          size_t selected,
          size_t offset) {
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

  for (size_t i = 0; i < visible; ++i) {
    size_t idx = offset + i;
    if (idx >= items.size()) {
      AppendStyledLine(&out, "", width, padding, panel_bg + muted);
      continue;
    }
    std::string prefix = idx == selected ? "> " : "  ";
    std::string line = prefix + items[idx].display;
    if (idx == selected) {
      AppendStyledLine(&out, line, width, padding, select_bg + bright + bold);
    } else {
      AppendStyledLine(&out, line, width, padding, panel_bg + text);
    }
  }

  AppendStyledLine(&out, rule, width, padding, header_bg + muted);
  std::string status = "Up/Down move  Enter confirm  Esc cancel  ";
  status += std::to_string(selected + 1) + "/" + std::to_string(items.size());
  AppendStyledLine(&out, status, width, padding, header_bg + muted);
  return WriteAll(fd, out);
}

}  // namespace

PickResult RunPickTui(const std::vector<PickItem>& items,
                      const std::string& title,
                      std::size_t* index,
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
  if (!Draw(fd, items, title, selected, offset)) {
    return PickResult::kError;
  }

  while (true) {
    char c = 0;
    if (!ReadByte(fd, &c)) {
      continue;
    }

    if (c == 0x03) {
      return PickResult::kCanceled;
    }

    if (c == '\r' || c == '\n') {
      *index = selected;
      return PickResult::kSelected;
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

    Draw(fd, items, title, selected, offset);
  }
}
