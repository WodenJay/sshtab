#include "alias.h"
#include "history.h"
#include "normalize.h"
#include "tokenize.h"
#include "util.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

void ExpectTrue(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::cerr << "FAIL " << file << ":" << line << " " << expr << "\n";
    ++g_failures;
  }
}

template <typename T, typename U>
void ExpectEq(const T& a, const U& b, const char* expr, const char* file, int line) {
  if (!(a == b)) {
    std::cerr << "FAIL " << file << ":" << line << " " << expr << "\n";
    ++g_failures;
  }
}

#define EXPECT_TRUE(x) ExpectTrue((x), #x, __FILE__, __LINE__)
#define EXPECT_FALSE(x) ExpectTrue(!(x), "!(" #x ")", __FILE__, __LINE__)
#define EXPECT_EQ(a, b) ExpectEq((a), (b), #a " == " #b, __FILE__, __LINE__)

std::string MakeTempDir() {
  std::string templ = "/tmp/sshtab_testXXXXXX";
  std::vector<char> buf(templ.begin(), templ.end());
  buf.push_back('\0');
  char* dir = mkdtemp(buf.data());
  if (!dir) {
    return std::string();
  }
  return std::string(dir);
}

void CleanupDir(const std::string& dir) {
  if (dir.empty()) {
    return;
  }
  std::string data_dir = dir + "/sshtab";
  std::string history = data_dir + "/history.log";
  std::string commands = data_dir + "/commands.log";
  std::string aliases = data_dir + "/aliases.log";
  std::string command_aliases = data_dir + "/aliases_cmd.log";
  unlink(history.c_str());
  unlink(commands.c_str());
  unlink(aliases.c_str());
  unlink(command_aliases.c_str());
  rmdir(data_dir.c_str());
  rmdir(dir.c_str());
}

void TestBase64() {
  std::string out;
  std::string err;
  EXPECT_TRUE(Base64Decode(Base64Encode("ssh user@host"), &out, &err));
  EXPECT_EQ(out, "ssh user@host");
  EXPECT_TRUE(Base64Decode(Base64Encode(""), &out, &err));
  EXPECT_EQ(out, "");
  EXPECT_FALSE(Base64Decode("TQ=", &out, &err));
  EXPECT_FALSE(Base64Decode("====", &out, &err));
  EXPECT_FALSE(Base64Decode("!!!!", &out, &err));
}

void TestNormalize() {
  std::string out;
  EXPECT_TRUE(NormalizeSshCommand("ssh user@host", &out));
  EXPECT_EQ(out, "ssh user@host");
  EXPECT_TRUE(NormalizeSshCommand("  ssh  'user@host -p 22'  ", &out));
  EXPECT_EQ(out, "ssh user@host -p 22");
  EXPECT_FALSE(NormalizeSshCommand("scp host", &out));
  EXPECT_EQ(ExtractArgsFromCommand("ssh user@host"), "user@host");
  EXPECT_EQ(ExtractArgsFromCommand("ssh"), "");
}

void TestTokenize() {
  std::vector<std::string> out;
  std::string err;
  EXPECT_TRUE(TokenizeArgs("user@host -p 22", &out, &err));
  EXPECT_EQ(out.size(), static_cast<size_t>(3));
  EXPECT_EQ(out[0], "user@host");
  EXPECT_EQ(out[1], "-p");
  EXPECT_EQ(out[2], "22");

  out.clear();
  EXPECT_TRUE(TokenizeArgs("user@host -i 'id file' -J \"jump host\"", &out, &err));
  EXPECT_EQ(out.size(), static_cast<size_t>(5));
  EXPECT_EQ(out[2], "id file");
  EXPECT_EQ(out[4], "jump host");

  out.clear();
  EXPECT_FALSE(TokenizeArgs("user@host \"unterminated", &out, &err));

  EXPECT_TRUE(ContainsControlChars(std::string("a\nb")));
  EXPECT_TRUE(ContainsForbiddenMetachars("a|b"));
}

void TestHistoryAndAlias() {
  std::string temp = MakeTempDir();
  EXPECT_FALSE(temp.empty());
  if (temp.empty()) {
    return;
  }
  setenv("XDG_DATA_HOME", temp.c_str(), 1);

  std::string err;
  EXPECT_EQ(GetDataDir(&err), temp + "/sshtab");
  EXPECT_TRUE(AppendHistory("ssh host1", 0, &err));
  EXPECT_TRUE(AppendHistory("ssh host2", 0, &err));
  EXPECT_TRUE(AppendHistory("ssh host1", 0, &err));
  EXPECT_TRUE(AppendHistory("ssh host1", 1, &err));

  auto entries = LoadRecentUnique(10, &err);
  EXPECT_EQ(entries.size(), static_cast<size_t>(2));
  std::unordered_set<std::string> commands;
  for (const auto& e : entries) {
    commands.insert(e.command);
  }
  EXPECT_TRUE(commands.count("ssh host1") == 1);
  EXPECT_TRUE(commands.count("ssh host2") == 1);

  int removed = 0;
  EXPECT_TRUE(DeleteHistoryCommand("ssh host2", &removed, &err));
  EXPECT_EQ(removed, 1);

  entries = LoadRecentUnique(10, &err);
  EXPECT_EQ(entries.size(), static_cast<size_t>(1));
  if (!entries.empty()) {
    EXPECT_EQ(entries[0].command, "ssh host1");
  }

  std::unordered_map<std::string, std::string> aliases;
  EXPECT_TRUE(SetAliasForArgs("host1", "alias1", &err));
  EXPECT_TRUE(LoadAliases(&aliases, &err));
  EXPECT_EQ(aliases["host1"], "alias1");

  EXPECT_TRUE(AppendCommandHistory("ls -la", 0, &err));
  EXPECT_TRUE(AppendCommandHistory("ssh host1", 0, &err));
  EXPECT_TRUE(AppendCommandHistory("echo bad", 1, &err));

  auto command_entries = LoadRecentUniqueCommands(10, &err);
  bool found_ls = false;
  bool found_ssh = false;
  bool found_bad = false;
  for (const auto& e : command_entries) {
    if (e.command == "ls -la") {
      found_ls = true;
    }
    if (e.command == "ssh host1") {
      found_ssh = true;
    }
    if (e.command == "echo bad") {
      found_bad = true;
    }
  }
  EXPECT_TRUE(found_ls);
  EXPECT_TRUE(found_ssh);
  EXPECT_FALSE(found_bad);

  std::unordered_map<std::string, std::string> command_aliases;
  EXPECT_TRUE(SetAliasForCommand("ls -la", "list", &err));
  EXPECT_TRUE(LoadCommandAliases(&command_aliases, &err));
  EXPECT_EQ(command_aliases["ls -la"], "list");

  CleanupDir(temp);
}

}  // namespace

int main() {
  TestBase64();
  TestNormalize();
  TestTokenize();
  TestHistoryAndAlias();
  if (g_failures == 0) {
    std::cout << "OK\n";
  }
  return g_failures == 0 ? 0 : 1;
}
