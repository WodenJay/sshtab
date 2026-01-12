#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct PickItem {
  std::string display;
  std::string args;
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
