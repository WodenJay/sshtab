#pragma once

#include <string>
#include <vector>

bool TokenizeArgs(const std::string& input, std::vector<std::string>* out, std::string* err);
bool ContainsControlChars(const std::string& input);
bool ContainsForbiddenMetachars(const std::string& input);
