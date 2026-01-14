#include "alias.h"

#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{

  bool ReadAllFromFd(int fd, std::string *out, std::string *err)
  {
    if (!out)
    {
      if (err)
      {
        *err = "output pointer is null";
      }
      return false;
    }
    out->clear();
    if (lseek(fd, 0, SEEK_SET) < 0)
    {
      if (err)
      {
        *err = std::string("lseek failed: ") + std::strerror(errno);
      }
      return false;
    }
    char buf[4096];
    while (true)
    {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        if (err)
        {
          *err = std::string("read failed: ") + std::strerror(errno);
        }
        return false;
      }
      if (n == 0)
      {
        break;
      }
      out->append(buf, static_cast<size_t>(n));
    }
    return true;
  }

  bool WriteAllToFd(int fd, const std::string &data, std::string *err)
  {
    size_t offset = 0;
    while (offset < data.size())
    {
      ssize_t n = write(fd, data.data() + offset, data.size() - offset);
      if (n < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        if (err)
        {
          *err = std::string("write failed: ") + std::strerror(errno);
        }
        return false;
      }
      offset += static_cast<size_t>(n);
    }
    return true;
  }

  std::string DirnameFromPath(const std::string &path)
  {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
    {
      return ".";
    }
    if (slash == 0)
    {
      return "/";
    }
    return path.substr(0, slash);
  }

  void ParseAliasContent(const std::string &content, std::unordered_map<std::string, std::string> *aliases)
  {
    if (!aliases)
    {
      return;
    }
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line))
    {
      if (line.empty())
      {
        continue;
      }
      size_t tab = line.find('\t');
      if (tab == std::string::npos)
      {
        continue;
      }
      std::string key_b64 = line.substr(0, tab);
      std::string val_b64 = line.substr(tab + 1);
      std::string key;
      std::string val;
      std::string decode_err;
      if (!Base64Decode(key_b64, &key, &decode_err))
      {
        continue;
      }
      if (!Base64Decode(val_b64, &val, &decode_err))
      {
        continue;
      }
      if (key.empty())
      {
        continue;
      }
      if (val.empty())
      {
        aliases->erase(key);
      }
      else
      {
        (*aliases)[key] = val;
      }
    }
  }

  bool WriteAliases(int fd, const std::unordered_map<std::string, std::string> &aliases, std::string *err)
  {
    std::vector<std::pair<std::string, std::string>> items;
    items.reserve(aliases.size());
    for (const auto &kv : aliases)
    {
      items.emplace_back(kv.first, kv.second);
    }
    std::sort(items.begin(), items.end(),
              [](const auto &a, const auto &b)
              { return a.first < b.first; });

    std::string out;
    for (const auto &kv : items)
    {
      if (kv.first.empty() || kv.second.empty())
      {
        continue;
      }
      out += Base64Encode(kv.first);
      out += '\t';
      out += Base64Encode(kv.second);
      out += '\n';
    }

    return WriteAllToFd(fd, out, err);
  }

} // namespace

bool LoadAliases(std::unordered_map<std::string, std::string> *aliases, std::string *err)
{
  if (!aliases)
  {
    if (err)
    {
      *err = "aliases pointer is null";
    }
    return false;
  }
  aliases->clear();

  std::string path_err;
  std::string path = GetAliasPath(&path_err);
  if (path.empty())
  {
    if (err)
    {
      *err = path_err;
    }
    return false;
  }

  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
  {
    if (errno == ENOENT)
    {
      return true;
    }
    if (err)
    {
      *err = std::string("open failed: ") + std::strerror(errno);
    }
    return false;
  }

  bool ok = true;
  if (flock(fd, LOCK_SH) != 0)
  {
    if (err)
    {
      *err = std::string("flock failed: ") + std::strerror(errno);
    }
    ok = false;
  }

  std::string content;
  if (ok && !ReadAllFromFd(fd, &content, err))
  {
    ok = false;
  }

  if (ok)
  {
    ParseAliasContent(content, aliases);
  }

  flock(fd, LOCK_UN);
  close(fd);
  return ok;
}

bool SetAliasForArgs(const std::string &args, const std::string &alias, std::string *err)
{
  if (args.empty())
  {
    if (err)
    {
      *err = "args is empty";
    }
    return false;
  }

  std::string path_err;
  std::string dir = GetDataDir(&path_err);
  if (dir.empty())
  {
    if (err)
    {
      *err = path_err;
    }
    return false;
  }
  if (!EnsureDir(dir, err))
  {
    return false;
  }

  std::string path = dir + "/aliases.log";
  int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0)
  {
    if (err)
    {
      *err = std::string("open failed: ") + std::strerror(errno);
    }
    return false;
  }

  bool ok = true;
  if (flock(fd, LOCK_EX) != 0)
  {
    if (err)
    {
      *err = std::string("flock failed: ") + std::strerror(errno);
    }
    ok = false;
  }

  std::unordered_map<std::string, std::string> aliases;
  std::string content;
  if (ok && !ReadAllFromFd(fd, &content, err))
  {
    ok = false;
  }
  if (ok)
  {
    ParseAliasContent(content, &aliases);
  }

  if (ok)
  {
    if (alias.empty())
    {
      aliases.erase(args);
    }
    else
    {
      aliases[args] = alias;
    }
  }

  std::string dir_path = DirnameFromPath(path);
  std::string tmp_path;
  int tmp_fd = -1;
  if (ok)
  {
    std::string tmpl = dir_path + "/aliases.log.tmp.XXXXXX";
    std::vector<char> tmp_buf(tmpl.begin(), tmpl.end());
    tmp_buf.push_back('\0');
    tmp_fd = mkstemp(tmp_buf.data());
    if (tmp_fd < 0)
    {
      if (err)
      {
        *err = std::string("mkstemp failed: ") + std::strerror(errno);
      }
      ok = false;
    }
    else
    {
      tmp_path = tmp_buf.data();
    }
  }

  if (ok)
  {
    if (!WriteAliases(tmp_fd, aliases, err))
    {
      ok = false;
    }
  }

  if (ok)
  {
    if (fsync(tmp_fd) != 0)
    {
      if (err)
      {
        *err = std::string("fsync failed: ") + std::strerror(errno);
      }
      ok = false;
    }
  }

  if (ok)
  {
    if (rename(tmp_path.c_str(), path.c_str()) != 0)
    {
      if (err)
      {
        *err = std::string("rename failed: ") + std::strerror(errno);
      }
      ok = false;
    }
  }

  if (tmp_fd >= 0)
  {
    close(tmp_fd);
  }
  if (!tmp_path.empty() && !ok)
  {
    unlink(tmp_path.c_str());
  }

  flock(fd, LOCK_UN);
  close(fd);
  return ok;
}
