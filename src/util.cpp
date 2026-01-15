#include "util.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool MkdirIfMissing(const std::string& path, std::string* err) {
  if (path.empty()) {
    if (err) {
      *err = "empty directory path";
    }
    return false;
  }
  if (mkdir(path.c_str(), 0700) == 0) {
    return true;
  }
  if (errno == EEXIST) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      return true;
    }
  }
  if (err) {
    *err = std::string("mkdir failed: ") + std::strerror(errno);
  }
  return false;
}

}  // namespace

std::string GetDataDir(std::string* err) {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg && *xdg) {
    return std::string(xdg) + "/sshtab";
  }
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.local/share/sshtab";
  }
  if (err) {
    *err = "XDG_DATA_HOME and HOME are not set";
  }
  return std::string();
}

std::string GetHistoryPath(std::string* err) {
  std::string dir = GetDataDir(err);
  if (dir.empty()) {
    return std::string();
  }
  return dir + "/history.log";
}

std::string GetCommandHistoryPath(std::string* err) {
  std::string dir = GetDataDir(err);
  if (dir.empty()) {
    return std::string();
  }
  return dir + "/commands.log";
}

std::string GetAliasPath(std::string* err) {
  std::string dir = GetDataDir(err);
  if (dir.empty()) {
    return std::string();
  }
  return dir + "/aliases.log";
}

std::string GetCommandAliasPath(std::string* err) {
  std::string dir = GetDataDir(err);
  if (dir.empty()) {
    return std::string();
  }
  return dir + "/aliases_cmd.log";
}

bool EnsureDir(const std::string& path, std::string* err) {
  if (path.empty()) {
    if (err) {
      *err = "empty directory path";
    }
    return false;
  }

  std::string cur;
  if (!path.empty() && path[0] == '/') {
    cur = "/";
  }
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') {
      ++i;
    }
    if (i >= path.size()) {
      break;
    }
    size_t next = path.find('/', i);
    std::string part = path.substr(i, next == std::string::npos ? path.size() - i : next - i);
    if (!part.empty()) {
      if (!cur.empty() && cur.back() != '/') {
        cur += '/';
      }
      cur += part;
      if (!MkdirIfMissing(cur, err)) {
        return false;
      }
    }
    if (next == std::string::npos) {
      break;
    }
    i = next + 1;
  }
  return true;
}

std::string TrimSpace(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && IsSpace(s[start])) {
    ++start;
  }
  size_t end = s.size();
  while (end > start && IsSpace(s[end - 1])) {
    --end;
  }
  return s.substr(start, end - start);
}

std::string CollapseSpaces(const std::string& s) {
  std::string out;
  bool in_space = false;
  for (char c : s) {
    if (IsSpace(c)) {
      if (!in_space) {
        out.push_back(' ');
        in_space = true;
      }
      continue;
    }
    in_space = false;
    out.push_back(c);
  }
  return TrimSpace(out);
}

std::string Base64Encode(const std::string& input) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 2 < input.size()) {
    unsigned int n = (static_cast<unsigned char>(input[i]) << 16) |
                     (static_cast<unsigned char>(input[i + 1]) << 8) |
                     static_cast<unsigned char>(input[i + 2]);
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back(kTable[(n >> 6) & 0x3F]);
    out.push_back(kTable[n & 0x3F]);
    i += 3;
  }

  size_t rem = input.size() - i;
  if (rem == 1) {
    unsigned int n = static_cast<unsigned char>(input[i]) << 16;
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    unsigned int n = (static_cast<unsigned char>(input[i]) << 16) |
                     (static_cast<unsigned char>(input[i + 1]) << 8);
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back(kTable[(n >> 6) & 0x3F]);
    out.push_back('=');
  }

  return out;
}

bool Base64Decode(const std::string& input, std::string* output, std::string* err) {
  if (!output) {
    if (err) {
      *err = "output pointer is null";
    }
    return false;
  }

  if (input.size() % 4 != 0) {
    if (err) {
      *err = "invalid base64 length";
    }
    return false;
  }

  auto decode_char = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };

  std::string out;
  out.reserve((input.size() / 4) * 3);

  int val = 0;
  int valb = -8;
  int pad = 0;
  bool padding = false;
  for (size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    if (c == '=') {
      padding = true;
      ++pad;
      continue;
    }
    if (padding) {
      if (err) {
        *err = "invalid base64 padding";
      }
      return false;
    }
    int d = decode_char(c);
    if (d < 0) {
      if (err) {
        *err = "invalid base64 character";
      }
      return false;
    }
    val = (val << 6) | d;
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }

  if (pad > 2) {
    if (err) {
      *err = "invalid base64 padding";
    }
    return false;
  }

  *output = out;
  return true;
}

ScopedFd::~ScopedFd() { reset(); }

ScopedFd::ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

ScopedFd& ScopedFd::operator=(ScopedFd&& other) noexcept {
  if (this != &other) {
    reset();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

int ScopedFd::release() {
  int fd = fd_;
  fd_ = -1;
  return fd;
}

void ScopedFd::reset(int fd) {
  if (fd_ >= 0) {
    while (close(fd_) != 0 && errno == EINTR) {
    }
  }
  fd_ = fd;
}

FlockGuard::~FlockGuard() { Unlock(); }

void FlockGuard::reset(int fd) {
  Unlock();
  fd_ = fd;
}

bool FlockGuard::LockExclusive(std::string* err) {
  if (fd_ < 0) {
    if (err) {
      *err = "invalid fd for flock";
    }
    return false;
  }
  while (flock(fd_, LOCK_EX) != 0) {
    if (errno == EINTR) {
      continue;
    }
    if (err) {
      *err = std::string("flock failed: ") + std::strerror(errno);
    }
    return false;
  }
  locked_ = true;
  return true;
}

bool FlockGuard::LockShared(std::string* err) {
  if (fd_ < 0) {
    if (err) {
      *err = "invalid fd for flock";
    }
    return false;
  }
  while (flock(fd_, LOCK_SH) != 0) {
    if (errno == EINTR) {
      continue;
    }
    if (err) {
      *err = std::string("flock failed: ") + std::strerror(errno);
    }
    return false;
  }
  locked_ = true;
  return true;
}

void FlockGuard::Unlock() {
  if (!locked_ || fd_ < 0) {
    return;
  }
  flock(fd_, LOCK_UN);
  locked_ = false;
}

bool ReadAllFromFd(int fd, std::string* out, std::string* err) {
  if (!out) {
    if (err) {
      *err = "output pointer is null";
    }
    return false;
  }
  out->clear();
  while (lseek(fd, 0, SEEK_SET) < 0) {
    if (errno == EINTR) {
      continue;
    }
    if (err) {
      *err = std::string("lseek failed: ") + std::strerror(errno);
    }
    return false;
  }
  char buf[4096];
  while (true) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (err) {
        *err = std::string("read failed: ") + std::strerror(errno);
      }
      return false;
    }
    if (n == 0) {
      break;
    }
    out->append(buf, static_cast<size_t>(n));
  }
  return true;
}

bool WriteAllToFd(int fd, const std::string& data, std::string* err) {
  size_t offset = 0;
  while (offset < data.size()) {
    ssize_t n = write(fd, data.data() + offset, data.size() - offset);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (err) {
        *err = std::string("write failed: ") + std::strerror(errno);
      }
      return false;
    }
    if (n == 0) {
      if (err) {
        *err = "write failed: short write";
      }
      return false;
    }
    offset += static_cast<size_t>(n);
  }
  return true;
}

std::string DirnameFromPath(const std::string& path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

bool FsyncDir(const std::string& dir, std::string* err) {
  int flags = O_RDONLY;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  int fd = open(dir.c_str(), flags);
  if (fd < 0) {
    if (err) {
      *err = std::string("open dir failed: ") + std::strerror(errno);
    }
    return false;
  }
  ScopedFd fd_guard(fd);
  if (fsync(fd) != 0) {
    if (err) {
      *err = std::string("fsync dir failed: ") + std::strerror(errno);
    }
    return false;
  }
  return true;
}
