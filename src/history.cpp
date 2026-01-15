#include "history.h"

#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
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

bool AppendHistoryToPath(const std::string& path,
                         const std::string& command,
                         int exit_code,
                         std::string* err) {
  if (path.empty()) {
    if (err) {
      *err = "history path is empty";
    }
    return false;
  }

  std::string dir = DirnameFromPath(path);
  if (dir.empty()) {
    if (err) {
      *err = "history directory is empty";
    }
    return false;
  }
  if (!EnsureDir(dir, err)) {
    return false;
  }

  int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (err) {
      *err = std::string("open failed: ") + std::strerror(errno);
    }
    return false;
  }

  ScopedFd fd_guard(fd);
  FlockGuard lock(fd);
  if (!lock.LockExclusive(err)) {
    return false;
  }

  std::time_t now = std::time(nullptr);
  std::string encoded = Base64Encode(command);
  std::ostringstream oss;
  oss << static_cast<long long>(now) << '\t' << exit_code << '\t' << encoded << '\n';
  std::string line = oss.str();
  return WriteAllToFd(fd, line, err);
}

std::vector<HistoryEntry> LoadRecentUniqueFromPath(const std::string& path,
                                                   std::size_t limit,
                                                   std::string* err) {
  std::vector<HistoryEntry> result;
  if (path.empty()) {
    if (err) {
      *err = "history path is empty";
    }
    return result;
  }

  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      return result;
    }
    if (err) {
      *err = std::string("open failed: ") + std::strerror(errno);
    }
    return result;
  }

  ScopedFd fd_guard(fd);
  FlockGuard lock(fd);
  if (!lock.LockShared(err)) {
    return result;
  }

  std::string content;
  if (!ReadAllFromFd(fd, &content, err)) {
    return result;
  }

  std::unordered_map<std::string, HistoryEntry> seen;
  std::istringstream in(content);
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

}  // namespace

bool AppendHistory(const std::string& command, int exit_code, std::string* err) {
  std::string path_err;
  std::string path = GetHistoryPath(&path_err);
  if (path.empty()) {
    if (err) {
      *err = path_err;
    }
    return false;
  }
  return AppendHistoryToPath(path, command, exit_code, err);
}

bool AppendCommandHistory(const std::string& command, int exit_code, std::string* err) {
  std::string path_err;
  std::string path = GetCommandHistoryPath(&path_err);
  if (path.empty()) {
    if (err) {
      *err = path_err;
    }
    return false;
  }
  return AppendHistoryToPath(path, command, exit_code, err);
}

std::vector<HistoryEntry> LoadRecentUnique(std::size_t limit, std::string* err) {
  std::string path_err;
  std::string path = GetHistoryPath(&path_err);
  if (path.empty()) {
    if (err) {
      *err = path_err;
    }
    return std::vector<HistoryEntry>();
  }
  return LoadRecentUniqueFromPath(path, limit, err);
}

std::vector<HistoryEntry> LoadRecentUniqueCommands(std::size_t limit, std::string* err) {
  std::string path_err;
  std::string path = GetCommandHistoryPath(&path_err);
  if (path.empty()) {
    if (err) {
      *err = path_err;
    }
    return std::vector<HistoryEntry>();
  }
  return LoadRecentUniqueFromPath(path, limit, err);
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

  int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    if (err) {
      *err = std::string("open failed: ") + std::strerror(errno);
    }
    return false;
  }

  ScopedFd fd_guard(fd);
  FlockGuard lock(fd);
  if (!lock.LockExclusive(err)) {
    return false;
  }

  std::string content;
  if (!ReadAllFromFd(fd, &content, err)) {
    return false;
  }

  std::string dir = DirnameFromPath(path);
  std::string tmp_path;
  ScopedFd tmp_guard;
  {
    std::string tmpl = dir + "/history.log.tmp.XXXXXX";
    std::vector<char> tmp_buf(tmpl.begin(), tmpl.end());
    tmp_buf.push_back('\0');
    int tmp_fd = mkstemp(tmp_buf.data());
    if (tmp_fd < 0) {
      if (err) {
        *err = std::string("mkstemp failed: ") + std::strerror(errno);
      }
      return false;
    }
    tmp_path = tmp_buf.data();
    tmp_guard.reset(tmp_fd);
  }

  int removed_count = 0;
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
      if (!WriteAllToFd(tmp_guard.get(), out_line, err)) {
        unlink(tmp_path.c_str());
        return false;
      }
    }
  }

  if (removed_count == 0) {
    if (err) {
      *err = "entry not found";
    }
    unlink(tmp_path.c_str());
    return false;
  }

  if (fsync(tmp_guard.get()) != 0) {
    if (err) {
      *err = std::string("fsync failed: ") + std::strerror(errno);
    }
    unlink(tmp_path.c_str());
    return false;
  }

  if (rename(tmp_path.c_str(), path.c_str()) != 0) {
    if (err) {
      *err = std::string("rename failed: ") + std::strerror(errno);
    }
    unlink(tmp_path.c_str());
    return false;
  }

  if (!FsyncDir(dir, err)) {
    return false;
  }

  if (removed) {
    *removed = removed_count;
  }
  return true;
}
