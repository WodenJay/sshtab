#include "history.h"
#include "normalize.h"
#include "tokenize.h"
#include "tui.h"

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
            << "  sshtab list --limit <N> [--with-ids]\n"
            << "  sshtab pick --limit <N> [--non-interactive --select <idx>]\n"
            << "  sshtab delete --index <N> [--limit <N>]\n"
            << "  sshtab delete --pick [--limit <N>]\n"
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
  bool with_ids = false;
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--limit") {
      if (i + 1 >= argc || !ParseSizeArg(argv[i + 1], &limit)) {
        std::cerr << "Invalid --limit value\n";
        return 1;
      }
      ++i;
    } else if (arg == "--with-ids") {
      with_ids = true;
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

  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (with_ids) {
      std::cout << i << '\t' << entries[i].command << "\n";
    } else {
      std::cout << entries[i].command << "\n";
    }
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

  std::string err;
  std::vector<HistoryEntry> entries = LoadRecentUnique(limit, &err);
  std::vector<PickItem> items;
  items.reserve(entries.size());
  for (const auto& entry : entries) {
    if (HasControlChars(entry.command)) {
      continue;
    }
    std::string args = ExtractArgsFromCommand(entry.command);
    if (args.empty() || HasControlChars(args)) {
      continue;
    }
    PickItem item;
    item.display = entry.command;
    item.args = args;
    items.push_back(item);
  }

  if (items.empty()) {
    return 1;
  }

  if (non_interactive) {
    if (select_idx < 0) {
      std::cerr << "--select is required in --non-interactive mode\n";
      return 1;
    }
    if (static_cast<std::size_t>(select_idx) >= items.size()) {
      return 1;
    }
    const std::string& args = items[select_idx].args;
    if (HasControlChars(args)) {
      return 1;
    }
    std::cout << args << "\n";
    return 0;
  }

  std::size_t selected = 0;
  PickResult result = RunPickTui(items, "sshtab pick (Enter select, Esc/Ctrl+C cancel)", &selected,
                                 &err);
  if (result == PickResult::kSelected) {
    if (selected >= items.size()) {
      return 1;
    }
    const std::string& args = items[selected].args;
    if (HasControlChars(args)) {
      return 1;
    }
    std::cout << args << "\n";
    return 0;
  }
  if (result == PickResult::kError && !err.empty()) {
    std::cerr << "pick failed: " << err << "\n";
  }
  return 1;
}

int CommandDelete(int argc, char** argv) {
  std::size_t limit = 50;
  bool use_pick = false;
  int index = -1;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--limit") {
      if (i + 1 >= argc || !ParseSizeArg(argv[i + 1], &limit)) {
        std::cerr << "Invalid --limit value\n";
        return 1;
      }
      ++i;
    } else if (arg == "--pick") {
      use_pick = true;
    } else if (arg == "--index") {
      if (i + 1 >= argc || !ParseIntArg(argv[i + 1], &index)) {
        std::cerr << "Invalid --index value\n";
        return 1;
      }
      ++i;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      return 1;
    }
  }

  if (use_pick && index >= 0) {
    std::cerr << "--pick and --index are mutually exclusive\n";
    return 1;
  }
  if (!use_pick && index < 0) {
    std::cerr << "--index or --pick is required\n";
    return 1;
  }

  std::string err;
  std::vector<HistoryEntry> entries = LoadRecentUnique(limit, &err);
  if (entries.empty()) {
    std::cerr << "delete failed: history is empty\n";
    return 1;
  }

  std::string command;
  if (use_pick) {
    std::vector<PickItem> items;
    std::vector<std::string> commands;
    items.reserve(entries.size());
    commands.reserve(entries.size());
    for (const auto& entry : entries) {
      if (HasControlChars(entry.command)) {
        continue;
      }
      PickItem item;
      item.display = entry.command;
      item.args = entry.command;
      items.push_back(item);
      commands.push_back(entry.command);
    }
    if (items.empty()) {
      std::cerr << "delete failed: no deletable entries\n";
      return 1;
    }
    std::size_t selected = 0;
    PickResult result =
        RunPickTui(items, "sshtab delete (Enter delete, Esc/Ctrl+C cancel)", &selected, &err);
    if (result != PickResult::kSelected) {
      return 1;
    }
    if (selected >= commands.size()) {
      return 1;
    }
    command = commands[selected];
  } else {
    if (index < 0) {
      std::cerr << "Invalid --index value\n";
      return 1;
    }
    if (static_cast<std::size_t>(index) >= entries.size()) {
      std::cerr << "delete failed: index out of range\n";
      return 1;
    }
    command = entries[static_cast<std::size_t>(index)].command;
  }

  int removed = 0;
  if (!DeleteHistoryCommand(command, &removed, &err)) {
    std::cerr << "delete failed: " << err << "\n";
    return 1;
  }
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
  if (cmd == "delete") {
    return CommandDelete(argc, argv);
  }
  if (cmd == "exec") {
    return CommandExec(argc, argv);
  }

  PrintUsage();
  return 1;
}
