#include "history.h"
#include "normalize.h"
#include "tokenize.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

void PrintUsage() {
  std::cerr << "Usage:\n"
            << "  sshtab record --exit-code <int> --raw <raw_cmd>\n"
            << "  sshtab list --limit <N>\n"
            << "  sshtab pick --limit <N> [--non-interactive --select <idx>]\n"
            << "  sshtab exec <args_string>\n";
}

bool ParseIntArg(const char* arg, int* out) {
  if (!arg || !out) {
    return false;
  }
  char* end = nullptr;
  long v = std::strtol(arg, &end, 10);
  if (!end || *end != '\0') {
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

bool ParseSizeArg(const char* arg, std::size_t* out) {
  if (!arg || !out) {
    return false;
  }
  char* end = nullptr;
  unsigned long v = std::strtoul(arg, &end, 10);
  if (!end || *end != '\0') {
    return false;
  }
  *out = static_cast<std::size_t>(v);
  return true;
}

bool HasControlChars(const std::string& s) {
  return ContainsControlChars(s);
}

int CommandRecord(int argc, char** argv) {
  int exit_code = -1;
  std::string raw;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--exit-code") {
      if (i + 1 >= argc || !ParseIntArg(argv[i + 1], &exit_code)) {
        std::cerr << "Invalid --exit-code value\n";
        return 1;
      }
      ++i;
    } else if (arg == "--raw") {
      if (i + 1 >= argc) {
        std::cerr << "Missing --raw value\n";
        return 1;
      }
      raw = argv[++i];
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      return 1;
    }
  }

  if (exit_code < 0) {
    std::cerr << "--exit-code is required\n";
    return 1;
  }
  if (raw.empty()) {
    std::cerr << "--raw is required\n";
    return 1;
  }

  if (exit_code != 0) {
    return 0;
  }

  std::string normalized;
  if (!NormalizeSshCommand(raw, &normalized)) {
    return 0;
  }

  std::string err;
  if (!AppendHistory(normalized, exit_code, &err)) {
    std::cerr << "record failed: " << err << "\n";
    return 1;
  }
  return 0;
}

int CommandList(int argc, char** argv) {
  std::size_t limit = 50;
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--limit") {
      if (i + 1 >= argc || !ParseSizeArg(argv[i + 1], &limit)) {
        std::cerr << "Invalid --limit value\n";
        return 1;
      }
      ++i;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      return 1;
    }
  }

  std::string err;
  std::vector<HistoryEntry> entries = LoadRecentUnique(limit, &err);
  if (!err.empty() && entries.empty()) {
    std::cerr << "list warning: " << err << "\n";
  }

  for (const auto& entry : entries) {
    std::cout << entry.command << "\n";
  }
  return 0;
}

int CommandPick(int argc, char** argv) {
  std::size_t limit = 50;
  bool non_interactive = false;
  int select_idx = -1;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--limit") {
      if (i + 1 >= argc || !ParseSizeArg(argv[i + 1], &limit)) {
        std::cerr << "Invalid --limit value\n";
        return 1;
      }
      ++i;
    } else if (arg == "--non-interactive") {
      non_interactive = true;
    } else if (arg == "--select") {
      if (i + 1 >= argc || !ParseIntArg(argv[i + 1], &select_idx)) {
        std::cerr << "Invalid --select value\n";
        return 1;
      }
      ++i;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      return 1;
    }
  }

  if (!non_interactive) {
    std::cerr << "interactive pick not implemented in milestone 1\n";
    return 2;
  }

  if (select_idx < 0) {
    std::cerr << "--select is required in --non-interactive mode\n";
    return 1;
  }

  std::string err;
  std::vector<HistoryEntry> entries = LoadRecentUnique(limit, &err);
  if (entries.empty()) {
    return 1;
  }
  if (static_cast<std::size_t>(select_idx) >= entries.size()) {
    return 1;
  }

  std::string args = ExtractArgsFromCommand(entries[select_idx].command);
  if (HasControlChars(args)) {
    return 1;
  }

  std::cout << args << "\n";
  return 0;
}

int CommandExec(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "exec requires exactly one args_string\n";
    return 1;
  }

  std::string args_string = argv[2];
  if (ContainsControlChars(args_string)) {
    std::cerr << "exec rejected control characters\n";
    return 1;
  }
  if (ContainsForbiddenMetachars(args_string)) {
    std::cerr << "exec rejected shell metacharacters\n";
    return 1;
  }

  std::vector<std::string> tokens;
  std::string tok_err;
  if (!TokenizeArgs(args_string, &tokens, &tok_err)) {
    std::cerr << "exec tokenize failed: " << tok_err << "\n";
    return 1;
  }

  std::vector<char*> argv_exec;
  argv_exec.reserve(tokens.size() + 2);
  argv_exec.push_back(const_cast<char*>("ssh"));
  for (auto& t : tokens) {
    argv_exec.push_back(const_cast<char*>(t.c_str()));
  }
  argv_exec.push_back(nullptr);

  execvp("ssh", argv_exec.data());
  std::cerr << "exec failed: " << std::strerror(errno) << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  std::string cmd = argv[1];
  if (cmd == "record") {
    return CommandRecord(argc, argv);
  }
  if (cmd == "list") {
    return CommandList(argc, argv);
  }
  if (cmd == "pick") {
    return CommandPick(argc, argv);
  }
  if (cmd == "exec") {
    return CommandExec(argc, argv);
  }

  PrintUsage();
  return 1;
}
