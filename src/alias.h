#pragma once

#include <string>
#include <unordered_map>

bool LoadAliases(std::unordered_map<std::string, std::string>* aliases, std::string* err);
bool SetAliasForArgs(const std::string& args, const std::string& alias, std::string* err);
