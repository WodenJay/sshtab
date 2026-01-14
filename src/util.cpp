#include "util.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

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

std::string GetAliasPath(std::string* err) {
  std::string dir = GetDataDir(err);
  if (dir.empty()) {
    return std::string();
  }
  return dir + "/aliases.log";
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
  for (char c : input) {
    if (c == '=') {
      ++pad;
      continue;
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
