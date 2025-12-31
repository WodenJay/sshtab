#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HistoryEntry {
  std::string command;
  std::int64_t last_used = 0;
  int count = 0;
};

bool AppendHistory(const std::string& command, int exit_code, std::string* err);
std::vector<HistoryEntry> LoadRecentUnique(std::size_t limit, std::string* err);
