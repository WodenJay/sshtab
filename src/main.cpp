#include "alias.h"
#include "history.h"
#include "normalize.h"
#include "tokenize.h"
#include "tui.h"
#include "util.h"

#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace
{

  struct SshMeta
  {
    std::string host;
    std::string port;
    std::string jump;
    std::string identity;
  };

  void PrintUsage()
  {
    std::cerr << "Usage:\n"
              << "  sshtab record --exit-code <int> --raw <raw_cmd>\n"
              << "    Record a successful ssh command from hooks.\n"
              << "  sshtab add <command...>\n"
              << "    Add a command to general history without executing.\n"
              << "  sshtab list --limit <N> [--with-ids]\n"
              << "    List recent ssh commands.\n"
              << "  sshtab pick --limit <N> [--non-interactive --select <idx>]\n"
              << "    Pick ssh args for completion.\n"
              << "  sshtab pick-command --limit <N> [--non-interactive --select <idx>]\n"
              << "    Pick full command lines for sshtab completion.\n"
              << "  sshtab alias --name <alias> (--id <N> [--limit <N>] | --address <addr>)\n"
              << "    Set or clear ssh alias display name.\n"
              << "  sshtab delete --index <N> [--limit <N>]\n"
              << "  sshtab delete --pick [--limit <N>]\n"
              << "    Delete ssh history entries.\n"
              << "  sshtab exec <args_string>\n"
              << "    Execute ssh with safe tokenization.\n";
  }

  bool ParseIntArg(const char *arg, int *out)
  {
    if (!arg || !out)
    {
      return false;
    }
    int value = 0;
    const char *end = arg + std::strlen(arg);
    auto result = std::from_chars(arg, end, value, 10);
    if (result.ec != std::errc() || result.ptr != end)
    {
      return false;
    }
    *out = value;
    return true;
  }

  bool ParseSizeArg(const char *arg, std::size_t *out)
  {
    if (!arg || !out)
    {
      return false;
    }
    if (arg[0] == '-')
    {
      return false;
    }
    unsigned long long value = 0;
    const char *end = arg + std::strlen(arg);
    auto result = std::from_chars(arg, end, value, 10);
    if (result.ec != std::errc() || result.ptr != end)
    {
      return false;
    }
    if (value > std::numeric_limits<std::size_t>::max())
    {
      return false;
    }
    *out = static_cast<std::size_t>(value);
    return true;
  }

  bool HasControlChars(const std::string &s)
  {
    return ContainsControlChars(s);
  }

  bool NeedsSingleQuote(const std::string &arg)
  {
    if (arg.empty())
    {
      return true;
    }
    for (unsigned char c : arg)
    {
      if (std::isspace(c) || c == '\'')
      {
        return true;
      }
    }
    return false;
  }

  std::string QuoteSingle(const std::string &arg)
  {
    std::string out = "'";
    size_t start = 0;
    while (start < arg.size())
    {
      size_t pos = arg.find('\'', start);
      if (pos == std::string::npos)
      {
        out += arg.substr(start);
        break;
      }
      out += arg.substr(start, pos - start);
      out += "'\\''";
      start = pos + 1;
    }
    out += "'";
    return out;
  }

  bool NormalizeCommandTokens(const std::vector<std::string> &tokens, std::string *out, std::string *err)
  {
    if (!out)
    {
      if (err)
      {
        *err = "command output pointer is null";
      }
      return false;
    }
    if (tokens.empty())
    {
      if (err)
      {
        *err = "command is empty";
      }
      return false;
    }

    std::string command;
    for (const auto &token : tokens)
    {
      if (ContainsControlChars(token))
      {
        if (err)
        {
          *err = "command contains control characters";
        }
        return false;
      }
      if (ContainsForbiddenMetachars(token))
      {
        if (err)
        {
          *err = "command contains shell metacharacters";
        }
        return false;
      }
      std::string part = NeedsSingleQuote(token) ? QuoteSingle(token) : token;
      if (!command.empty())
      {
        command += ' ';
      }
      command += part;
    }
    if (command.empty())
    {
      if (err)
      {
        *err = "command is empty";
      }
      return false;
    }
    *out = command;
    return true;
  }

  bool NormalizeCommandRaw(const std::string &input, std::string *out, std::string *err)
  {
    if (!out)
    {
      if (err)
      {
        *err = "command output pointer is null";
      }
      return false;
    }
    std::string trimmed = TrimSpace(input);
    if (trimmed.empty())
    {
      if (err)
      {
        *err = "command is empty";
      }
      return false;
    }
    if (ContainsControlChars(trimmed))
    {
      if (err)
      {
        *err = "command contains control characters";
      }
      return false;
    }
    if (ContainsForbiddenMetachars(trimmed))
    {
      if (err)
      {
        *err = "command contains shell metacharacters";
      }
      return false;
    }
    *out = trimmed;
    return true;
  }

  std::vector<std::string> SplitArgsSimple(const std::string &args)
  {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < args.size())
    {
      while (i < args.size() && args[i] == ' ')
      {
        ++i;
      }
      if (i >= args.size())
      {
        break;
      }
      size_t j = i;
      while (j < args.size() && args[j] != ' ')
      {
        ++j;
      }
      out.push_back(args.substr(i, j - i));
      i = j;
    }
    return out;
  }

  std::string BasenamePath(const std::string &path)
  {
    if (path.empty())
    {
      return path;
    }
    size_t end = path.size();
    while (end > 0 && path[end - 1] == '/')
    {
      --end;
    }
    if (end == 0)
    {
      return path;
    }
    size_t slash = path.rfind('/', end - 1);
    if (slash == std::string::npos)
    {
      return path.substr(0, end);
    }
    return path.substr(slash + 1, end - slash - 1);
  }

  SshMeta ExtractSshMeta(const std::string &args)
  {
    SshMeta meta;
    std::vector<std::string> tokens = SplitArgsSimple(args);
    for (size_t i = 0; i < tokens.size(); ++i)
    {
      const std::string &tok = tokens[i];
      if (tok == "-p")
      {
        if (i + 1 < tokens.size())
        {
          meta.port = tokens[++i];
        }
        continue;
      }
      if (tok.size() > 2 && tok.rfind("-p", 0) == 0)
      {
        meta.port = tok.substr(2);
        continue;
      }
      if (tok == "-J")
      {
        if (i + 1 < tokens.size())
        {
          meta.jump = tokens[++i];
        }
        continue;
      }
      if (tok.size() > 2 && tok.rfind("-J", 0) == 0)
      {
        meta.jump = tok.substr(2);
        continue;
      }
      if (tok == "-i")
      {
        if (i + 1 < tokens.size())
        {
          meta.identity = BasenamePath(tokens[++i]);
        }
        continue;
      }
      if (tok.size() > 2 && tok.rfind("-i", 0) == 0)
      {
        meta.identity = BasenamePath(tok.substr(2));
        continue;
      }
      if (!tok.empty() && tok[0] == '-')
      {
        continue;
      }
      meta.host = tok;
    }
    return meta;
  }

  bool NormalizeArgsInput(const std::string &input, std::string *out, std::string *err)
  {
    if (!out)
    {
      if (err)
      {
        *err = "args output pointer is null";
      }
      return false;
    }
    std::string trimmed = TrimSpace(input);
    if (trimmed.empty())
    {
      if (err)
      {
        *err = "args is empty";
      }
      return false;
    }

    std::string normalized;
    if (NormalizeSshCommand(trimmed, &normalized))
    {
      std::string args = ExtractArgsFromCommand(normalized);
      if (args.empty())
      {
        if (err)
        {
          *err = "args is empty";
        }
        return false;
      }
      *out = args;
      return true;
    }

    std::string args = CollapseSpaces(trimmed);
    if (args.empty())
    {
      if (err)
      {
        *err = "args is empty";
      }
      return false;
    }
    *out = args;
    return true;
  }

  bool NormalizeAliasName(const std::string &input, std::string *out, std::string *err)
  {
    if (!out)
    {
      if (err)
      {
        *err = "alias output pointer is null";
      }
      return false;
    }
    std::string trimmed = TrimSpace(input);
    if (ContainsControlChars(trimmed))
    {
      if (err)
      {
        *err = "alias contains control characters";
      }
      return false;
    }
    *out = trimmed;
    return true;
  }

  int CommandRecord(int argc, char **argv)
  {
    int exit_code = -1;
    std::string raw;

    for (int i = 2; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--exit-code")
      {
        if (i + 1 >= argc || !ParseIntArg(argv[i + 1], &exit_code))
        {
          std::cerr << "Invalid --exit-code value\n";
          return 1;
        }
        ++i;
      }
      else if (arg == "--raw")
      {
        if (i + 1 >= argc)
        {
          std::cerr << "Missing --raw value\n";
          return 1;
        }
        raw = argv[++i];
      }
      else
      {
        std::cerr << "Unknown argument: " << arg << "\n";
        return 1;
      }
    }

    if (exit_code < 0)
    {
      std::cerr << "--exit-code is required\n";
      return 1;
    }
    if (raw.empty())
    {
      std::cerr << "--raw is required\n";
      return 1;
    }

    if (ContainsControlChars(raw))
    {
      return 0;
    }

    if (exit_code != 0)
    {
      return 0;
    }

    std::string normalized;
    if (!NormalizeSshCommand(raw, &normalized))
    {
      return 0;
    }

    std::string err;
    if (!AppendHistory(normalized, exit_code, &err))
    {
      std::cerr << "record failed: " << err << "\n";
      return 1;
    }
    err.clear();
    if (!AppendCommandHistory(normalized, exit_code, &err))
    {
      std::cerr << "record failed: " << err << "\n";
      return 1;
    }
    return 0;
  }

  int CommandAdd(int argc, char **argv)
  {
    if (argc < 3)
    {
      std::cerr << "add requires a command" << "\n";
      return 1;
    }

    std::string command;
    std::string err;
    if (argc == 3)
    {
      if (!NormalizeCommandRaw(argv[2], &command, &err))
      {
        std::cerr << "add failed: " << err << "\n";
        return 1;
      }
    }
    else
    {
      std::vector<std::string> tokens;
      tokens.reserve(static_cast<std::size_t>(argc - 2));
      for (int i = 2; i < argc; ++i)
      {
        tokens.emplace_back(argv[i]);
      }
      if (!NormalizeCommandTokens(tokens, &command, &err))
      {
        std::cerr << "add failed: " << err << "\n";
        return 1;
      }
    }

    if (command.size() >= 7 && command.rfind("sshtab ", 0) == 0)
    {
      command = command.substr(7);
      size_t first = command.find_first_not_of(' ');
      if (first == std::string::npos)
      {
        std::cerr << "add failed: command empty after stripping sshtab prefix\n";
        return 1;
      }
      command = command.substr(first);
    }

    if (!AppendCommandHistory(command, 0, &err))
    {
      std::cerr << "add failed: " << err << "\n";
      return 1;
    }
    return 0;
  }

  int CommandList(int argc, char **argv)
  {
    std::size_t limit = 50;
    bool with_ids = false;
    for (int i = 2; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--limit")
      {
        if (i + 1 >= argc || !ParseSizeArg(argv[i + 1], &limit))
        {
          std::cerr << "Invalid --limit value\n";
          return 1;
        }
        ++i;
      }
      else if (arg == "--with-ids")
      {
        with_ids = true;
      }
      else
      {
        std::cerr << "Unknown argument: " << arg << "\n";
        return 1;
      }
    }

    std::string err;
    std::vector<HistoryEntry> entries = LoadRecentUnique(limit, &err);
    if (!err.empty() && entries.empty())
    {
      std::cerr << "list warning: " << err << "\n";
    }

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
      if (with_ids)
      {
        std::cout << i << '\t' << entries[i].command << "\n";
      }
      else
      {
        std::cout << entries[i].command << "\n";
      }
    }
    return 0;
  }

  int CommandPick(int argc, char **argv)
  {
    std::size_t limit = 50;
    bool non_interactive = false;
    int select_idx = -1;

    for (int i = 2; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--limit")
      {
        if (i + 1 >= argc || !ParseSizeArg(argv[i + 1], &limit))
        {
          std::cerr << "Invalid --limit value\n";
          return 1;
        }
        ++i;
      }
      else if (arg == "--non-interactive")
      {
        non_interactive = true;
      }
      else if (arg == "--select")
      {
        if (i + 1 >= argc || !ParseIntArg(argv[i + 1], &select_idx))
        {
          std::cerr << "Invalid --select value\n";
          return 1;
        }
        ++i;
      }
      else
      {
        std::cerr << "Unknown argument: " << arg << "\n";
        return 1;
      }
    }

    std::string err;
    std::vector<HistoryEntry> entries = LoadRecentUnique(limit, &err);
    std::unordered_map<std::string, std::string> aliases;
    std::string alias_err;
    LoadAliases(&aliases, &alias_err);
    std::vector<PickItem> items;
    items.reserve(entries.size());
    for (const auto &entry : entries)
    {
      if (HasControlChars(entry.command))
      {
        continue;
      }
      std::string args = ExtractArgsFromCommand(entry.command);
      if (args.empty() || HasControlChars(args))
      {
        continue;
      }
      SshMeta meta = ExtractSshMeta(args);
      PickItem item;
      item.display = entry.command;
      auto alias_it = aliases.find(args);
      if (alias_it != aliases.end() && !HasControlChars(alias_it->second))
      {
        item.alias = alias_it->second;
      }
      item.args = args;
      item.last_used = entry.last_used;
      item.count = entry.count;
      item.host = meta.host;
      item.port = meta.port;
      item.jump = meta.jump;
      item.identity = meta.identity;
      items.push_back(item);
    }

    if (items.empty())
    {
      return 1;
    }

    if (non_interactive)
    {
      if (select_idx < 0)
      {
        std::cerr << "--select is required in --non-interactive mode\n";
        return 1;
      }
      if (static_cast<std::size_t>(select_idx) >= items.size())
      {
        return 1;
      }
      const std::string &args = items[select_idx].args;
      if (HasControlChars(args))
      {
        return 1;
      }
      std::cout << args << "\n";
      return 0;
    }

    PickUiConfig config;
    config.allow_alias_edit = true;
    config.allow_display_toggle = true;
    config.show_alias = true;

    AliasUpdateFn alias_update = [&](const PickItem &item, const std::string &alias_input,
                                     std::string *out_err) -> bool
    {
      std::string alias;
      std::string alias_err;
      if (!NormalizeAliasName(alias_input, &alias, &alias_err))
      {
        if (out_err)
        {
          *out_err = alias_err;
        }
        return false;
      }
      if (ContainsControlChars(item.args))
      {
        if (out_err)
        {
          *out_err = "args contain control characters";
        }
        return false;
      }
      return SetAliasForArgs(item.args, alias, out_err);
    };

    std::size_t selected = 0;
    PickResult result = RunPickTui(items, "sshtab pick (Enter select, Esc/Ctrl+C cancel)", &selected,
                                   config, alias_update, &err);
    if (result == PickResult::kSelected)
    {
      if (selected >= items.size())
      {
        return 1;
      }
      const std::string &args = items[selected].args;
      if (HasControlChars(args))
      {
        return 1;
      }
      std::cout << args << "\n";
      return 0;
    }
    if (result == PickResult::kError && !err.empty())
    {
      std::cerr << "pick failed: " << err << "\n";
    }
    return 1;
  }

  int CommandPickCommand(int argc, char **argv)
  {
    std::size_t limit = 50;
    bool non_interactive = false;
    int select_idx = -1;

    for (int i = 2; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--limit")
      {
        if (i + 1 >= argc || !ParseSizeArg(argv[i + 1], &limit))
        {
          std::cerr << "Invalid --limit value\n";
          return 1;
        }
        ++i;
      }
      else if (arg == "--non-interactive")
      {
        non_interactive = true;
      }
      else if (arg == "--select")
      {
        if (i + 1 >= argc || !ParseIntArg(argv[i + 1], &select_idx))
        {
          std::cerr << "Invalid --select value\n";
          return 1;
        }
        ++i;
      }
      else
      {
        std::cerr << "Unknown argument: " << arg << "\n";
        return 1;
      }
    }

    std::string command_err;
    std::vector<HistoryEntry> command_entries = LoadRecentUniqueCommands(limit, &command_err);
    if (!command_err.empty() && command_entries.empty())
    {
      std::cerr << "pick-command warning: " << command_err << "\n";
    }

    std::string ssh_err;
    std::vector<HistoryEntry> ssh_entries = LoadRecentUnique(limit, &ssh_err);
    if (!ssh_err.empty() && ssh_entries.empty())
    {
      std::cerr << "pick-command warning: " << ssh_err << "\n";
    }

    std::unordered_map<std::string, HistoryEntry> merged;
    for (const auto &entry : command_entries)
    {
      merged.emplace(entry.command, entry);
    }
    for (const auto &entry : ssh_entries)
    {
      if (merged.find(entry.command) == merged.end())
      {
        merged.emplace(entry.command, entry);
      }
    }

    std::vector<HistoryEntry> entries;
    entries.reserve(merged.size());
    for (const auto &kv : merged)
    {
      entries.push_back(kv.second);
    }

    std::sort(entries.begin(), entries.end(), [](const HistoryEntry &a, const HistoryEntry &b)
              {
                if (a.last_used != b.last_used)
                {
                  return a.last_used > b.last_used;
                }
                if (a.count != b.count)
                {
                  return a.count > b.count;
                }
                return a.command < b.command;
              });

    if (limit > 0 && entries.size() > limit)
    {
      entries.resize(limit);
    }

    std::unordered_map<std::string, std::string> command_aliases;
    std::string alias_err;
    LoadCommandAliases(&command_aliases, &alias_err);

    std::unordered_map<std::string, std::string> ssh_aliases;
    std::string ssh_alias_err;
    LoadAliases(&ssh_aliases, &ssh_alias_err);

    std::vector<PickItem> items;
    items.reserve(entries.size());
    for (const auto &entry : entries)
    {
      if (HasControlChars(entry.command) || ContainsForbiddenMetachars(entry.command))
      {
        continue;
      }
      PickItem item;
      item.display = entry.command;
      item.args = entry.command;
      auto alias_it = command_aliases.find(entry.command);
      if (alias_it != command_aliases.end() && !HasControlChars(alias_it->second))
      {
        item.alias = alias_it->second;
      }
      if (item.alias.empty())
      {
        std::string args = ExtractArgsFromCommand(entry.command);
        if (!args.empty())
        {
          auto ssh_alias_it = ssh_aliases.find(args);
          if (ssh_alias_it != ssh_aliases.end() && !HasControlChars(ssh_alias_it->second))
          {
            item.alias = ssh_alias_it->second;
          }
        }
      }
      std::string args = ExtractArgsFromCommand(entry.command);
      if (!args.empty())
      {
        SshMeta meta = ExtractSshMeta(args);
        item.host = meta.host;
        item.port = meta.port;
        item.jump = meta.jump;
        item.identity = meta.identity;
      }
      item.last_used = entry.last_used;
      item.count = entry.count;
      items.push_back(item);
    }

    if (items.empty())
    {
      return 1;
    }

    if (non_interactive)
    {
      if (select_idx < 0)
      {
        std::cerr << "--select is required in --non-interactive mode\n";
        return 1;
      }
      if (static_cast<std::size_t>(select_idx) >= items.size())
      {
        return 1;
      }
      const std::string &command = items[select_idx].args;
      if (HasControlChars(command) || ContainsForbiddenMetachars(command))
      {
        return 1;
      }
      std::cout << command << "\n";
      return 0;
    }

    PickUiConfig config;
    config.allow_alias_edit = true;
    config.allow_display_toggle = true;
    config.show_alias = true;

    AliasUpdateFn alias_update = [&](const PickItem &item, const std::string &alias_input,
                                     std::string *out_err) -> bool
    {
      std::string alias;
      std::string alias_err;
      if (!NormalizeAliasName(alias_input, &alias, &alias_err))
      {
        if (out_err)
        {
          *out_err = alias_err;
        }
        return false;
      }
      if (ContainsControlChars(item.args) || ContainsForbiddenMetachars(item.args))
      {
        if (out_err)
        {
          *out_err = "command contains invalid characters";
        }
        return false;
      }
      return SetAliasForCommand(item.args, alias, out_err);
    };

    std::string err;
    std::size_t selected = 0;
    PickResult result = RunPickTui(items, "sshtab pick-command (Enter select, Esc/Ctrl+C cancel)",
                                   &selected, config, alias_update, &err);
    if (result == PickResult::kSelected)
    {
      if (selected >= items.size())
      {
        return 1;
      }
      const std::string &command = items[selected].args;
      if (HasControlChars(command) || ContainsForbiddenMetachars(command))
      {
        return 1;
      }
      std::cout << command << "\n";
      return 0;
    }
    if (result == PickResult::kError && !err.empty())
    {
      std::cerr << "pick-command failed: " << err << "\n";
    }
    return 1;
  }

  int CommandAlias(int argc, char **argv)
  {
    std::size_t limit = 50;
    int id = -1;
    std::string address;
    std::string name;
    bool have_name = false;

    for (int i = 2; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--id")
      {
        if (i + 1 >= argc || !ParseIntArg(argv[i + 1], &id))
        {
          std::cerr << "Invalid --id value\n";
          return 1;
        }
        ++i;
      }
      else if (arg == "--address")
      {
        if (i + 1 >= argc)
        {
          std::cerr << "Missing --address value\n";
          return 1;
        }
        address = argv[++i];
      }
      else if (arg == "--name")
      {
        if (i + 1 >= argc)
        {
          std::cerr << "Missing --name value\n";
          return 1;
        }
        name = argv[++i];
        have_name = true;
      }
      else if (arg == "--limit")
      {
        if (i + 1 >= argc || !ParseSizeArg(argv[i + 1], &limit))
        {
          std::cerr << "Invalid --limit value\n";
          return 1;
        }
        ++i;
      }
      else
      {
        std::cerr << "Unknown argument: " << arg << "\n";
        return 1;
      }
    }

    if (!have_name)
    {
      std::cerr << "--name is required\n";
      return 1;
    }
    if (id >= 0 && !address.empty())
    {
      std::cerr << "--id and --address are mutually exclusive\n";
      return 1;
    }
    if (id < 0 && address.empty())
    {
      std::cerr << "--id or --address is required\n";
      return 1;
    }

    std::string alias;
    std::string alias_err;
    if (!NormalizeAliasName(name, &alias, &alias_err))
    {
      std::cerr << "alias failed: " << alias_err << "\n";
      return 1;
    }

    std::string args;
    std::string args_err;
    if (id >= 0)
    {
      std::string err;
      std::vector<HistoryEntry> entries = LoadRecentUnique(limit, &err);
      if (entries.empty())
      {
        std::cerr << "alias failed: history is empty\n";
        return 1;
      }
      if (static_cast<std::size_t>(id) >= entries.size())
      {
        std::cerr << "alias failed: id out of range\n";
        return 1;
      }
      args = ExtractArgsFromCommand(entries[static_cast<std::size_t>(id)].command);
      if (args.empty())
      {
        std::cerr << "alias failed: empty args\n";
        return 1;
      }
    }
    else
    {
      if (!NormalizeArgsInput(address, &args, &args_err))
      {
        std::cerr << "alias failed: " << args_err << "\n";
        return 1;
      }
    }

    if (ContainsControlChars(args))
    {
      std::cerr << "alias failed: args contain control characters\n";
      return 1;
    }

    std::string err;
    if (!SetAliasForArgs(args, alias, &err))
    {
      std::cerr << "alias failed: " << err << "\n";
      return 1;
    }
    return 0;
  }

  int CommandDelete(int argc, char **argv)
  {
    std::size_t limit = 50;
    bool use_pick = false;
    int index = -1;

    for (int i = 2; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--limit")
      {
        if (i + 1 >= argc || !ParseSizeArg(argv[i + 1], &limit))
        {
          std::cerr << "Invalid --limit value\n";
          return 1;
        }
        ++i;
      }
      else if (arg == "--pick")
      {
        use_pick = true;
      }
      else if (arg == "--index")
      {
        if (i + 1 >= argc || !ParseIntArg(argv[i + 1], &index))
        {
          std::cerr << "Invalid --index value\n";
          return 1;
        }
        ++i;
      }
      else
      {
        std::cerr << "Unknown argument: " << arg << "\n";
        return 1;
      }
    }

    if (use_pick && index >= 0)
    {
      std::cerr << "--pick and --index are mutually exclusive\n";
      return 1;
    }
    if (!use_pick && index < 0)
    {
      std::cerr << "--index or --pick is required\n";
      return 1;
    }

    std::string err;
    std::vector<HistoryEntry> entries = LoadRecentUnique(limit, &err);
    if (entries.empty())
    {
      std::cerr << "delete failed: history is empty\n";
      return 1;
    }

    std::unordered_map<std::string, std::string> aliases;
    std::string alias_err;
    LoadAliases(&aliases, &alias_err);

    std::string command;
    if (use_pick)
    {
      std::vector<PickItem> items;
      std::vector<std::string> commands;
      items.reserve(entries.size());
      commands.reserve(entries.size());
      for (const auto &entry : entries)
      {
        if (HasControlChars(entry.command))
        {
          continue;
        }
        std::string args = ExtractArgsFromCommand(entry.command);
        SshMeta meta = ExtractSshMeta(args);
        PickItem item;
        item.display = entry.command;
        auto alias_it = aliases.find(args);
        if (alias_it != aliases.end() && !HasControlChars(alias_it->second))
        {
          item.alias = alias_it->second;
        }
        item.args = args;
        item.last_used = entry.last_used;
        item.count = entry.count;
        item.host = meta.host;
        item.port = meta.port;
        item.jump = meta.jump;
        item.identity = meta.identity;
        items.push_back(item);
        commands.push_back(entry.command);
      }
      if (items.empty())
      {
        std::cerr << "delete failed: no deletable entries\n";
        return 1;
      }
      PickUiConfig config;
      config.allow_alias_edit = false;
      config.allow_display_toggle = true;
      config.show_alias = true;
      std::size_t selected = 0;
      PickResult result = RunPickTui(items, "sshtab delete (Enter delete, Esc/Ctrl+C cancel)",
                                     &selected, config, AliasUpdateFn(), &err);
      if (result != PickResult::kSelected)
      {
        return 1;
      }
      if (selected >= commands.size())
      {
        return 1;
      }
      command = commands[selected];
    }
    else
    {
      if (index < 0)
      {
        std::cerr << "Invalid --index value\n";
        return 1;
      }
      if (static_cast<std::size_t>(index) >= entries.size())
      {
        std::cerr << "delete failed: index out of range\n";
        return 1;
      }
      command = entries[static_cast<std::size_t>(index)].command;
    }

    int removed = 0;
    if (!DeleteHistoryCommand(command, &removed, &err))
    {
      std::cerr << "delete failed: " << err << "\n";
      return 1;
    }
    return 0;
  }

  int CommandExec(int argc, char **argv)
  {
    if (argc != 3)
    {
      std::cerr << "exec requires exactly one args_string\n";
      return 1;
    }

    std::string args_string = argv[2];
    if (ContainsControlChars(args_string))
    {
      std::cerr << "exec rejected control characters\n";
      return 1;
    }
    if (ContainsForbiddenMetachars(args_string))
    {
      std::cerr << "exec rejected shell metacharacters\n";
      return 1;
    }

    std::vector<std::string> tokens;
    std::string tok_err;
    if (!TokenizeArgs(args_string, &tokens, &tok_err))
    {
      std::cerr << "exec tokenize failed: " << tok_err << "\n";
      return 1;
    }

    std::vector<std::string> argv_storage;
    argv_storage.reserve(tokens.size() + 1);
    argv_storage.emplace_back("ssh");
    for (const auto &t : tokens)
    {
      argv_storage.push_back(t);
    }

    std::vector<char *> argv_exec;
    argv_exec.reserve(argv_storage.size() + 1);
    for (auto &arg : argv_storage)
    {
      argv_exec.push_back(arg.data());
    }
    argv_exec.push_back(nullptr);

    execvp(argv_exec[0], argv_exec.data());
    std::cerr << "exec failed: " << std::strerror(errno) << "\n";
    return 1;
  }

} // namespace

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    PrintUsage();
    return 1;
  }

  std::string cmd = argv[1];
  if (cmd == "record")
  {
    return CommandRecord(argc, argv);
  }
  if (cmd == "add")
  {
    return CommandAdd(argc, argv);
  }
  if (cmd == "list")
  {
    return CommandList(argc, argv);
  }
  if (cmd == "pick")
  {
    return CommandPick(argc, argv);
  }
  if (cmd == "pick-command")
  {
    return CommandPickCommand(argc, argv);
  }
  if (cmd == "alias")
  {
    return CommandAlias(argc, argv);
  }
  if (cmd == "delete")
  {
    return CommandDelete(argc, argv);
  }
  if (cmd == "exec")
  {
    return CommandExec(argc, argv);
  }

  PrintUsage();
  return 1;
}
