#include "tokenize.h"

#include <cctype>

bool ContainsControlChars(const std::string& input) {
  for (unsigned char c : input) {
    if (c < 0x20 || c == 0x7F) {
      return true;
    }
  }
  return false;
}

bool ContainsForbiddenMetachars(const std::string& input) {
  for (char c : input) {
    switch (c) {
      case ';':
      case '|':
      case '&':
      case '`':
      case '$':
      case '(':
      case ')':
      case '<':
      case '>':
        return true;
      default:
        break;
    }
  }
  return false;
}

bool TokenizeArgs(const std::string& input, std::vector<std::string>* out, std::string* err) {
  if (!out) {
    if (err) {
      *err = "output pointer is null";
    }
    return false;
  }
  out->clear();

  enum class State {
    kNormal,
    kSingle,
    kDouble,
  };

  State state = State::kNormal;
  std::string cur;

  for (size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    if (state == State::kNormal) {
      if (c == '\\') {
        if (i + 1 < input.size()) {
          cur.push_back(input[++i]);
        } else {
          cur.push_back('\\');
        }
        continue;
      }
      if (c == '\'') {
        state = State::kSingle;
        continue;
      }
      if (c == '"') {
        state = State::kDouble;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(c))) {
        if (!cur.empty()) {
          out->push_back(cur);
          cur.clear();
        }
        continue;
      }
      cur.push_back(c);
      continue;
    }

    if (state == State::kSingle) {
      if (c == '\'') {
        state = State::kNormal;
      } else {
        cur.push_back(c);
      }
      continue;
    }

    if (state == State::kDouble) {
      if (c == '"') {
        state = State::kNormal;
      } else if (c == '\\') {
        if (i + 1 < input.size()) {
          cur.push_back(input[++i]);
        } else {
          cur.push_back('\\');
        }
      } else {
        cur.push_back(c);
      }
      continue;
    }
  }

  if (state != State::kNormal) {
    if (err) {
      *err = "unterminated quote";
    }
    return false;
  }

  if (!cur.empty()) {
    out->push_back(cur);
  }
  return true;
}
