#pragma once

#include <cstdint>
#include <string>

std::string GetDataDir(std::string* err);
std::string GetHistoryPath(std::string* err);
std::string GetCommandHistoryPath(std::string* err);
std::string GetAliasPath(std::string* err);
std::string GetCommandAliasPath(std::string* err);
bool EnsureDir(const std::string& path, std::string* err);

std::string TrimSpace(const std::string& s);
std::string CollapseSpaces(const std::string& s);

std::string Base64Encode(const std::string& input);
bool Base64Decode(const std::string& input, std::string* output, std::string* err);

class ScopedFd {
 public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {}
  ~ScopedFd();
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&& other) noexcept;
  ScopedFd& operator=(ScopedFd&& other) noexcept;

  int get() const { return fd_; }
  int release();
  void reset(int fd = -1);

 private:
  int fd_;
};

class FlockGuard {
 public:
  explicit FlockGuard(int fd = -1) : fd_(fd) {}
  ~FlockGuard();
  FlockGuard(const FlockGuard&) = delete;
  FlockGuard& operator=(const FlockGuard&) = delete;

  void reset(int fd);
  bool LockExclusive(std::string* err);
  bool LockShared(std::string* err);
  void Unlock();

 private:
  int fd_;
  bool locked_ = false;
};

bool ReadAllFromFd(int fd, std::string* out, std::string* err);
bool WriteAllToFd(int fd, const std::string& data, std::string* err);
std::string DirnameFromPath(const std::string& path);
bool FsyncDir(const std::string& dir, std::string* err);
