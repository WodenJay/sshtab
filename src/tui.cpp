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

size_t GetVisibleCount(size_t total) {
  const size_t kDefault = 10;
  return total < kDefault ? total : kDefault;
}

size_t GetTerminalWidth(int fd) {
  struct winsize ws;
  if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return static_cast<size_t>(ws.ws_col);
  }
  return 80;
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

void AppendClearedLine(std::string* out, const std::string& text, size_t width, bool highlight) {
  if (!out) {
    return;
  }
  std::string line = TruncateLine(text, width);
  out->append("\r\x1b[2K");
  if (highlight) {
    out->append("\x1b[7m");
  }
  out->append(line);
  if (highlight && line.size() < width) {
    out->append(width - line.size(), ' ');
  }
  if (highlight) {
    out->append("\x1b[0m");
  }
  out->append("\n");
}

bool Draw(int fd, const std::vector<PickItem>& items, size_t selected, size_t offset) {
  const size_t width = GetTerminalWidth(fd);
  std::string out;
  out += "\x1b[2J\x1b[H";
  AppendClearedLine(&out, "sshtab pick (Enter select, Esc/Ctrl+C cancel)", width, false);
  size_t visible = GetVisibleCount(items.size());
  for (size_t i = 0; i < visible; ++i) {
    size_t idx = offset + i;
    if (idx >= items.size()) {
      AppendClearedLine(&out, "", width, false);
      continue;
    }
    AppendClearedLine(&out, items[idx].display, width, idx == selected);
  }
  return WriteAll(fd, out);
}

}  // namespace

PickResult RunPickTui(const std::vector<PickItem>& items, std::size_t* index, std::string* err) {
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
  if (!Draw(fd, items, selected, offset)) {
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

    size_t visible = GetVisibleCount(items.size());
    if (selected < offset) {
      offset = selected;
    } else if (selected >= offset + visible) {
      offset = selected - visible + 1;
    }

    Draw(fd, items, selected, offset);
  }
}
