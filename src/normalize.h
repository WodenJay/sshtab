#pragma once

#include <string>

bool NormalizeSshCommand(const std::string& raw, std::string* out);
std::string ExtractArgsFromCommand(const std::string& command);
