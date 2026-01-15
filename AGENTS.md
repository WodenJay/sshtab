# SSHTab Agent Guide

## Purpose
- Build `sshtab`, a Bash-first helper for `ssh<Tab>` history selection.
- Provide a safe `sshtab exec` path that never shells out.
- Record only successful `ssh` commands for reuse.

## Scope
- C++17 CLI with subcommands: `record`, `list`, `pick`, `exec`, `alias`, `delete`.
- Bash integration: completion, proxy `ssh()`, pre/post hooks.
- Install/uninstall scripts that manage a marked bashrc snippet.
- Minimal dependencies (no fzf/ncurses/sqlite).

## Non-goals (v1)
- Support shells other than Bash.
- Execute arbitrary shell expressions.
- Persist data outside XDG user paths.

## Runtime flow
1) User types `ssh` and presses Tab.
2) Bash completion runs `sshtab pick`.
3) `pick` shows TUI via `/dev/tty`, returns args on stdout.
4) Completion inserts a single quoted arg: `ssh '<args>'`.
5) User presses Enter; proxy `ssh()` calls `sshtab exec "<args>"`.
6) `exec` tokenizes args and `execvp` into real `ssh`.
7) Post-hook records if exit code is 0 and command was `ssh`.

## Project map
- `src/main.cpp`: CLI parsing + command handlers.
- `src/history.{h,cpp}`: history log append/read/dedupe/delete.
- `src/alias.{h,cpp}`: alias storage for args display.
- `src/normalize.{h,cpp}`: normalize raw ssh strings.
- `src/tokenize.{h,cpp}`: arg tokenizer + safety checks.
- `src/tui.{h,cpp}`: /dev/tty TUI selector.
- `src/util.{h,cpp}`: XDG paths, base64, file/lock helpers.
- `bash/sshtab.bash`: proxy function, hooks, completion.
- `scripts/*.sh`: install/uninstall helpers.
- `tests/test_main.cpp`: simple test runner.

## Build / test / lint
- Build: `make` (outputs `./sshtab`).
- Clean: `make clean`.
- Tests: `make test` or run `./sshtab_tests` directly.
- Single test: no filter support; temporarily call only one test in `tests/test_main.cpp`.
- Compiler: `g++` with `-std=c++17 -O2 -Wall -Wextra -pedantic`.
- Override compiler: `CXX=clang++ make`.
- Lint/format: no lint target or formatter configured; avoid reformatting.

## CLI command contracts
- `record --exit-code N --raw "<raw_cmd>"`: ignore unless exit code 0 and command is `ssh`.
- `list --limit N [--with-ids]`: prints one command per line.
- `pick --limit N [--non-interactive --select <idx>]`: stdout only selected args.
- `pick` must return non-zero on cancel or invalid output.
- `alias --name <alias> (--id <N> | --address <addr>)`: set or clear alias.
- `delete --index <N>` or `delete --pick`: remove history entries.
- `exec "<args_string>"`: tokenize and `execvp` real ssh.

## Data model
- Data dir: `${XDG_DATA_HOME:-$HOME/.local/share}/sshtab/`.
- History log: `history.log`.
- Alias log: `aliases.log`.
- History line format: `epoch<TAB>exit_code<TAB>base64(command_utf8)`.
- Alias line format: `base64(args)<TAB>base64(alias)`.
- Skip malformed lines or failed base64 decodes.
- Only exit code 0 entries are kept in history reads.

## Code style (C++)
- Language standard: C++17 only.
- Indentation: 2 spaces; no tabs.
- Braces: follow the file's existing style (K&R or Allman); do not reformat.
- Includes: own header first, then other project headers, then C++ stdlib, then POSIX.
- Naming:
- Types/structs/functions use `PascalCase`.
- Variables/parameters use `snake_case`.
- Constants use `kConstant` prefix.
- Enum classes use `kValue` enumerators.
- Prefer `const` where possible; pass by `const&` for large objects.
- Prefer `std::string`, `std::vector`, `std::size_t` over raw buffers.
- Avoid `using namespace`; qualify `std::`.
- Use `auto` only when the type is obvious from context.
- Keep functions small and cohesive; add helpers in the same module.
- Do not add inline comments unless necessary.
- Error handling: return `bool`/`int`, set `std::string* err` when provided.
- CLI errors: print to `std::cerr` with actionable messages.
- Always validate pointer arguments before dereferencing.
- For syscalls: check return codes, handle `EINTR`, and include `errno` details.
- Use RAII wrappers `ScopedFd` and `FlockGuard` for fd lifetime/locking.
- Avoid exceptions for control flow; functions should be noexcept by default.

## Shell style (scripts/snippet)
- Use `#!/usr/bin/env bash` and `set -euo pipefail`.
- Quote variable expansions; avoid unquoted globs.
- Prefer `[[ ... ]]` for tests and `case` for PATH checks.
- Avoid `eval` and external dependencies.
- Keep install/uninstall idempotent and guard with markers.
- Send user-facing errors to stderr.

## Bash integration rules (high priority)
- Trigger completion only when the line is exactly `ssh` or `ssh `.
- If current word is non-empty, return empty `COMPREPLY` to allow native completion.
- `pick` must not write to stdout during UI; only final args line allowed.
- Completion inserts a single quoted arg using the `\'\''` pattern.
- Do not use `$'...'` or any quoting that interprets escapes.
- Always set `compopt -o nospace` to prevent trailing spaces.
- Treat pick output as single-line payload; reject control chars/newlines.
- Completion mode: `SSHTAB_COMPLETION_MODE=always|fallback` (default fallback).
- Fallback mode: cancel/failure/empty history returns success with empty `COMPREPLY`.
- Resolve real ssh path (e.g., `type -P ssh`) before defining proxy.
- Cache result as `SSHTAB_REAL_SSH`; proxy uses `command "$SSHTAB_REAL_SSH" "$@"`.
- If real ssh cannot be resolved, disable proxy and warn once.
- Proxy `ssh()` only intercepts when exactly one argument contains spaces.
- Hooks only run in interactive shells; guard internal calls with `SSHTAB_GUARD=1`.
- `PROMPT_COMMAND` must prepend post-hook and capture `ec=$?` on first line.
- Composition must be idempotent; avoid duplicate hooks.

## DEBUG trap compatibility
- Do not override an existing DEBUG trap.
- If a DEBUG trap exists, set `SSHTAB_PREHOOK_ENABLED=0` and warn once.
- When pre-hook is disabled, post-hook must return immediately.
- Pre-hook must match command name strictly: `^ssh([[:space:]]|$)`.
- Pre-hook ignores internal `sshtab` helper calls (guarded).

## Exec/record safety rules
- `record` only writes when exit code is 0 and normalized command is `ssh`.
- `record` ignores inputs containing control characters.
- `exec` rejects control chars and shell metacharacters ``; | & \` $ ( ) < >``.
- `exec` uses tokenizer output directly and calls `execvp` (no shell).
- Reject pick output containing control characters or newlines.

## Install/uninstall rules
- Scripts must inject/remove only the marked bashrc snippet.
- Re-running install/uninstall must be idempotent.
- Uninstall may optionally remove data (`--purge` in scripts).
- Do not hardcode `/usr/bin/ssh`; resolve via PATH in bash snippet.

## Tests style
- Tests live in `tests/test_main.cpp` with simple `EXPECT_*` macros.
- Keep tests deterministic; use temp dirs under `/tmp`.
- Use `XDG_DATA_HOME` overrides for file system tests.
- Cleanup temp data on success/failure.
- Prefer small helper functions near the test that uses them.

## Assumptions
- `record` captures only plain `ssh` (no `sudo ssh`).
- `pick` output must be single line; newline/control chars cancel.
- TUI reads/writes only `/dev/tty`.
- Native ssh completion should remain available outside the exact trigger.

## Cursor/Copilot rules
- No `.cursor/rules`, `.cursorrules`, or `.github/copilot-instructions.md` found.

