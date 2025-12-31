#include "tui.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
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

struct TermiosGuard {
  int fd = -1;
  termios orig{};
  bool active = false;

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
    active = true;
    return true;
  }

  ~TermiosGuard() {
    if (active) {
      tcsetattr(fd, TCSAFLUSH, &orig);
    }
  }
};

struct CursorGuard {
  int fd = -1;
  bool hidden = false;
  ~CursorGuard() {
    if (hidden) {
      WriteAll(fd, "\x1b[?25h");
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

bool Draw(int fd, const std::vector<PickItem>& items, size_t selected, size_t offset) {
  std::string out;
  out += "\x1b[2J\x1b[H";
  out += "sshtab pick (Enter select, Esc/Ctrl+C cancel)\n";
  size_t visible = GetVisibleCount(items.size());
  for (size_t i = 0; i < visible; ++i) {
    size_t idx = offset + i;
    if (idx >= items.size()) {
      out += "\n";
      continue;
    }
    if (idx == selected) {
      out += "\x1b[7m";
    }
    out += items[idx].display;
    if (idx == selected) {
      out += "\x1b[0m";
    }
    out += "\n";
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

  TermiosGuard term;
  term.fd = fd;
  if (!term.EnableRaw(err)) {
    return PickResult::kError;
  }

  CursorGuard cursor;
  cursor.fd = fd;
  cursor.hidden = true;
  WriteAll(fd, "\x1b[?25l");

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
      WriteAll(fd, "\x1b[2J\x1b[H");
      return PickResult::kCanceled;
    }

    if (c == '\r' || c == '\n') {
      *index = selected;
      WriteAll(fd, "\x1b[2J\x1b[H");
      return PickResult::kSelected;
    }

    if (c == 0x1b) {
      char next = 0;
      if (!ReadByte(fd, &next)) {
        WriteAll(fd, "\x1b[2J\x1b[H");
        return PickResult::kCanceled;
      }
      if (next != '[') {
        WriteAll(fd, "\x1b[2J\x1b[H");
        return PickResult::kCanceled;
      }
      char code = 0;
      if (!ReadByte(fd, &code)) {
        WriteAll(fd, "\x1b[2J\x1b[H");
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
