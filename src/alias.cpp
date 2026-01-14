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

  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
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

  ScopedFd fd_guard(fd);
  FlockGuard lock(fd);
  if (!lock.LockShared(err))
  {
    return false;
  }

  std::string content;
  if (!ReadAllFromFd(fd, &content, err))
  {
    return false;
  }

  ParseAliasContent(content, aliases);
  return true;
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
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0)
  {
    if (err)
    {
      *err = std::string("open failed: ") + std::strerror(errno);
    }
    return false;
  }

  ScopedFd fd_guard(fd);
  FlockGuard lock(fd);
  if (!lock.LockExclusive(err))
  {
    return false;
  }

  std::unordered_map<std::string, std::string> aliases;
  std::string content;
  if (!ReadAllFromFd(fd, &content, err))
  {
    return false;
  }
  ParseAliasContent(content, &aliases);

  if (alias.empty())
  {
    aliases.erase(args);
  }
  else
  {
    aliases[args] = alias;
  }

  std::string dir_path = DirnameFromPath(path);
  std::string tmp_path;
  ScopedFd tmp_guard;
  {
    std::string tmpl = dir_path + "/aliases.log.tmp.XXXXXX";
    std::vector<char> tmp_buf(tmpl.begin(), tmpl.end());
    tmp_buf.push_back('\0');
    int tmp_fd = mkstemp(tmp_buf.data());
    if (tmp_fd < 0)
    {
      if (err)
      {
        *err = std::string("mkstemp failed: ") + std::strerror(errno);
      }
      return false;
    }
    tmp_path = tmp_buf.data();
    tmp_guard.reset(tmp_fd);
  }

  if (!WriteAliases(tmp_guard.get(), aliases, err))
  {
    return false;
  }

  if (fsync(tmp_guard.get()) != 0)
  {
    if (err)
    {
      *err = std::string("fsync failed: ") + std::strerror(errno);
    }
    return false;
  }

  if (rename(tmp_path.c_str(), path.c_str()) != 0)
  {
    if (err)
    {
      *err = std::string("rename failed: ") + std::strerror(errno);
    }
    unlink(tmp_path.c_str());
    return false;
  }

  if (!FsyncDir(dir_path, err))
  {
    return false;
  }
  return true;
}
