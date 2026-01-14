#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct PickItem {
  std::string display;
  std::string alias;
  std::string args;
  std::int64_t last_used = 0;
  int count = 0;
  std::string host;
  std::string port;
  std::string jump;
  std::string identity;
};

struct PickUiConfig {
  bool allow_alias_edit = false;
  bool allow_display_toggle = true;
  bool show_alias = false;
};

enum class PickResult {
  kSelected,
  kCanceled,
  kError,
};

using AliasUpdateFn =
    std::function<bool(const PickItem& item, const std::string& alias, std::string* err)>;

PickResult RunPickTui(std::vector<PickItem>& items,
                      const std::string& title,
                      std::size_t* index,
                      const PickUiConfig& config,
                      const AliasUpdateFn& alias_update,
                      std::string* err);
