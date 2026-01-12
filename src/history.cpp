#include "history.h"

#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

bool ParseInt64(const std::string& s, std::int64_t* out) {
  if (!out) {
    return false;
  }
  try {
    size_t idx = 0;
    long long v = std::stoll(s, &idx, 10);
    if (idx != s.size()) {
      return false;
    }
    *out = static_cast<std::int64_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseInt(const std::string& s, int* out) {
  if (!out) {
    return false;
  }
  try {
    size_t idx = 0;
    int v = std::stoi(s, &idx, 10);
    if (idx != s.size()) {
      return false;
    }
    *out = v;
    return true;
  } catch (...) {
    return false;
  }
}

bool ReadAllFromFd(int fd, std::string* out, std::string* err) {
  if (!out) {
    if (err) {
      *err = "output pointer is null";
    }
    return false;
  }
  out->clear();
  if (lseek(fd, 0, SEEK_SET) < 0) {
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

}  // namespace

bool AppendHistory(const std::string& command, int exit_code, std::string* err) {
  std::string path_err;
  std::string dir = GetDataDir(&path_err);
  if (dir.empty()) {
    if (err) {
      *err = path_err;
    }
    return false;
  }
  if (!EnsureDir(dir, err)) {
    return false;
  }

  std::string path = dir + "/history.log";
  int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
  if (fd < 0) {
    if (err) {
      *err = std::string("open failed: ") + std::strerror(errno);
    }
    return false;
  }

  bool ok = true;
  if (flock(fd, LOCK_EX) != 0) {
    if (err) {
      *err = std::string("flock failed: ") + std::strerror(errno);
    }
    ok = false;
  }

  if (ok) {
    std::time_t now = std::time(nullptr);
    std::string encoded = Base64Encode(command);
    std::ostringstream oss;
    oss << static_cast<long long>(now) << '\t' << exit_code << '\t' << encoded << '\n';
    std::string line = oss.str();
    ssize_t written = write(fd, line.data(), line.size());
    if (written < 0 || static_cast<size_t>(written) != line.size()) {
      if (err) {
        *err = std::string("write failed: ") + std::strerror(errno);
      }
      ok = false;
    }
  }

  flock(fd, LOCK_UN);
  close(fd);
  return ok;
}

std::vector<HistoryEntry> LoadRecentUnique(std::size_t limit, std::string* err) {
  std::vector<HistoryEntry> result;
  std::string path_err;
  std::string path = GetHistoryPath(&path_err);
  if (path.empty()) {
    if (err) {
      *err = path_err;
    }
    return result;
  }

  std::ifstream in(path);
  if (!in.is_open()) {
    return result;
  }

  std::unordered_map<std::string, HistoryEntry> seen;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    size_t t1 = line.find('\t');
    if (t1 == std::string::npos) {
      continue;
    }
    size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) {
      continue;
    }
    std::string ts_str = line.substr(0, t1);
    std::string code_str = line.substr(t1 + 1, t2 - t1 - 1);
    std::string b64 = line.substr(t2 + 1);

    std::int64_t ts = 0;
    int exit_code = 0;
    if (!ParseInt64(ts_str, &ts)) {
      continue;
    }
    if (!ParseInt(code_str, &exit_code)) {
      continue;
    }
    if (exit_code != 0) {
      continue;
    }

    std::string decoded;
    std::string decode_err;
    if (!Base64Decode(b64, &decoded, &decode_err)) {
      continue;
    }

    auto it = seen.find(decoded);
    if (it == seen.end()) {
      HistoryEntry entry;
      entry.command = decoded;
      entry.last_used = ts;
      entry.count = 1;
      seen.emplace(decoded, entry);
    } else {
      it->second.count += 1;
      if (ts > it->second.last_used) {
        it->second.last_used = ts;
      }
    }
  }

  result.reserve(seen.size());
  for (const auto& kv : seen) {
    result.push_back(kv.second);
  }

  std::sort(result.begin(), result.end(), [](const HistoryEntry& a, const HistoryEntry& b) {
    if (a.last_used != b.last_used) {
      return a.last_used > b.last_used;
    }
    if (a.count != b.count) {
      return a.count > b.count;
    }
    return a.command < b.command;
  });

  if (limit > 0 && result.size() > limit) {
    result.resize(limit);
  }

  return result;
}

bool DeleteHistoryCommand(const std::string& command, int* removed, std::string* err) {
  if (removed) {
    *removed = 0;
  }

  std::string path_err;
  std::string path = GetHistoryPath(&path_err);
  if (path.empty()) {
    if (err) {
      *err = path_err;
    }
    return false;
  }

  int fd = open(path.c_str(), O_RDWR);
  if (fd < 0) {
    if (err) {
      *err = std::string("open failed: ") + std::strerror(errno);
    }
    return false;
  }

  bool ok = true;
  if (flock(fd, LOCK_EX) != 0) {
    if (err) {
      *err = std::string("flock failed: ") + std::strerror(errno);
    }
    ok = false;
  }

  std::string content;
  if (ok && !ReadAllFromFd(fd, &content, err)) {
    ok = false;
  }

  std::string dir = DirnameFromPath(path);
  std::string tmp_path;
  int tmp_fd = -1;
  if (ok) {
    std::string tmpl = dir + "/history.log.tmp.XXXXXX";
    std::vector<char> tmp_buf(tmpl.begin(), tmpl.end());
    tmp_buf.push_back('\0');
    tmp_fd = mkstemp(tmp_buf.data());
    if (tmp_fd < 0) {
      if (err) {
        *err = std::string("mkstemp failed: ") + std::strerror(errno);
      }
      ok = false;
    } else {
      tmp_path = tmp_buf.data();
    }
  }

  int removed_count = 0;
  if (ok) {
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
      bool drop = false;
      size_t t1 = line.find('\t');
      size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
      if (t1 != std::string::npos && t2 != std::string::npos) {
        std::string b64 = line.substr(t2 + 1);
        std::string decoded;
        std::string decode_err;
        if (Base64Decode(b64, &decoded, &decode_err) && decoded == command) {
          drop = true;
          ++removed_count;
        }
      }

      if (!drop) {
        std::string out_line = line + "\n";
        if (!WriteAllToFd(tmp_fd, out_line, err)) {
          ok = false;
          break;
        }
      }
    }
  }

  if (ok && removed_count == 0) {
    if (err) {
      *err = "entry not found";
    }
    ok = false;
  }

  if (ok) {
    if (fsync(tmp_fd) != 0) {
      if (err) {
        *err = std::string("fsync failed: ") + std::strerror(errno);
      }
      ok = false;
    }
  }

  if (ok) {
    if (rename(tmp_path.c_str(), path.c_str()) != 0) {
      if (err) {
        *err = std::string("rename failed: ") + std::strerror(errno);
      }
      ok = false;
    }
  }

  if (tmp_fd >= 0) {
    close(tmp_fd);
  }
  if (!tmp_path.empty() && !ok) {
    unlink(tmp_path.c_str());
  }

  flock(fd, LOCK_UN);
  close(fd);

  if (removed) {
    *removed = removed_count;
  }
  return ok;
}
