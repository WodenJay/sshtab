#include "normalize.h"

#include "util.h"

namespace {

bool StartsWithSshToken(const std::string& s) {
  if (s.size() < 3) {
    return false;
  }
  if (s.compare(0, 3, "ssh") != 0) {
    return false;
  }
  if (s.size() == 3) {
    return true;
  }
  char next = s[3];
  return next == ' ' || next == '\t' || next == '\n' || next == '\r';
}

}  // namespace

bool NormalizeSshCommand(const std::string& raw, std::string* out) {
  if (!out) {
    return false;
  }
  std::string trimmed = TrimSpace(raw);
  if (trimmed.empty()) {
    return false;
  }
  if (!StartsWithSshToken(trimmed)) {
    return false;
  }

  std::string rest = trimmed.size() > 3 ? TrimSpace(trimmed.substr(3)) : std::string();
  if (rest.empty()) {
    *out = "ssh";
    return true;
  }

  std::string rest_trim = TrimSpace(rest);
  if (rest_trim.size() >= 2 &&
      (rest_trim.front() == '\'' || rest_trim.front() == '"') &&
      rest_trim.back() == rest_trim.front()) {
    rest = rest_trim.substr(1, rest_trim.size() - 2);
  } else {
    rest = rest_trim;
  }

  rest = CollapseSpaces(rest);
  if (rest.empty()) {
    *out = "ssh";
    return true;
  }

  *out = "ssh " + rest;
  return true;
}

std::string ExtractArgsFromCommand(const std::string& command) {
  std::string trimmed = TrimSpace(command);
  if (trimmed == "ssh") {
    return std::string();
  }
  if (trimmed.size() > 3 && trimmed.compare(0, 4, "ssh ") == 0) {
    return TrimSpace(trimmed.substr(4));
  }
  return std::string();
}
