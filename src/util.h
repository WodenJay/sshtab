#pragma once

#include <cstdint>
#include <string>

std::string GetDataDir(std::string* err);
std::string GetHistoryPath(std::string* err);
std::string GetAliasPath(std::string* err);
bool EnsureDir(const std::string& path, std::string* err);

std::string TrimSpace(const std::string& s);
std::string CollapseSpaces(const std::string& s);

std::string Base64Encode(const std::string& input);
bool Base64Decode(const std::string& input, std::string* output, std::string* err);
