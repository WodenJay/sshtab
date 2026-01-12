#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct PickItem {
  std::string display;
  std::string args;
  std::int64_t last_used = 0;
  int count = 0;
  std::string host;
  std::string port;
  std::string jump;
  std::string identity;
};

enum class PickResult {
  kSelected,
  kCanceled,
  kError,
};

PickResult RunPickTui(const std::vector<PickItem>& items,
                      const std::string& title,
                      std::size_t* index,
                      std::string* err);
