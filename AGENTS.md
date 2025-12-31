# SSHTab Agent Plan

## Purpose
Build `sshtab`, a Bash-first helper that provides interactive recent SSH selection on `ssh<Tab>`, safely executes `ssh '<args>'`, and records only successful SSH commands for history.

## Scope
- C++ CLI with subcommands: `record`, `list`, `pick`, `exec` (plus optional `gc` later).
- Bash integration: completion, proxy `ssh()` function, and hooks for record-on-success.
- Installer/uninstaller scripts that inject/remove a marked bashrc snippet.
- GitHub-ready project structure and release build pipeline.

## Non-Goals (v1)
- Support shells other than Bash.
- External dependencies (e.g., fzf, ncurses, sqlite).
- Executing arbitrary shell expressions through `exec`.

## Architecture Overview
### Runtime Flow
1) User types `ssh` and presses Tab.
2) Bash completion calls `sshtab pick`.
3) `pick` shows TUI via `/dev/tty`, returns selected args on stdout.
4) Completion inserts a single quoted argument: `ssh '<args>'`.
5) User hits Enter; proxy `ssh()` calls `sshtab exec "<args>"`.
6) `exec` tokenizes args safely and `execv` into the resolved real ssh path.
7) Debug trap captures raw command string; prompt hook records only if exit code is 0.

### Project Layout
- `src/main.cpp` CLI entry and dispatch.
- `src/history.{h,cpp}` read/append history, dedupe, sort.
- `src/normalize.{h,cpp}` normalize raw ssh commands.
- `src/tokenize.{h,cpp}` tokenize args string for `exec`.
- `src/tui.{h,cpp}` /dev/tty UI.
- `scripts/install.sh` install binary + bash integration.
- `scripts/uninstall.sh` remove integration.
- `bash/sshtab.bash` snippet with proxy, hooks, completion.
- `README.md` usage, install, uninstall, safety.
- `.github/workflows/release.yml` build artifacts.

## Subcommand Boundaries
### `sshtab record --exit-code N --raw "<raw_cmd>"`
- Input: raw command string (from Bash), exit code.
- Behavior: only record if exit code is 0 and command name is exactly `ssh`.
- Output: none; writes to history log with file lock.
- No UI; no stdout unless error.

### `sshtab list --limit N`
- Input: limit (default 50).
- Behavior: read history, dedupe by command, sort by last-used desc.
- Output: one command per line to stdout.
- No UI; no modification.

### `sshtab pick --limit N`
- Input: limit (default 50).
- Behavior: TUI selection in `/dev/tty`; stdout only selected args (without `ssh`).
- Output: args string or empty; non-zero exit on cancel.
- Output must be a single line of plain text; if the selected args contain newline or control characters, treat as cancel/failure and return non-zero.
- Must not pollute stdout during UI.

### `sshtab exec "<args_string>"`
- Input: single string that may contain spaces.
- Behavior: tokenize with a minimal shell-like parser (spaces, quotes, backslashes).
- Reject if contains shell metacharacters: `; | & \` $ ( ) < >`.
- Reject if input contains any control characters (ASCII < 0x20 or 0x7f), including `\n`, `\r`, and `\0`; return non-zero.
- Execute the resolved real ssh path with `execv` (no shell).

## Module Responsibilities
- `main`: argument parsing, dispatch, exit codes.
- `history`: XDG path resolution, file locking, append log, read and parse log, dedupe and sort.
- `normalize`: detect ssh invocation, trim whitespace, unwrap single/double quotes for `ssh '<args>'` forms.
- `tokenize`: split args string into argv tokens; no environment expansion or command substitution.
- `tui`: raw mode, key handling, filter, redraw; input/output via `/dev/tty`.
- `bash/sshtab.bash`: proxy function, hooks, completion; no side effects unless in interactive shell.

## Data Model
- Data dir: `${XDG_DATA_HOME:-$HOME/.local/share}/sshtab/`.
- Log file: `history.log`.
- Line format: `epoch_seconds<TAB>exit_code<TAB>base64(command_utf8)`.
- Parsing: skip malformed lines; only accept base64 decode success.

## Bash Integration Rules
- Resolve real ssh path before defining the `ssh()` proxy; do not resolve after the proxy is in scope.
- Resolution must return an executable file path (prefer `type -P ssh` or an equivalent that ignores aliases/functions).
- If resolution fails, the snippet must disable the `ssh()` proxy and avoid `sshtab exec` usage, and it must print a warning to the user (do not hard-fail or recurse).
- Cache the resolved path in a snippet variable (e.g., `SSHTAB_REAL_SSH`) and proxy `ssh()` must call `command "$SSHTAB_REAL_SSH" "$@"`.
- Proxy `ssh()` delegates to real ssh unless exactly one argument with spaces.
- Completion MUST trigger only when the input is exactly `ssh` or `ssh ` and the host-position word is empty.
- If the current word is non-empty (even one character), sshtab MUST return empty `COMPREPLY` and let native ssh completion handle it.
- Completion inserts a single quoted arg using single-quote-safe concatenation (e.g., `'\''` pattern) and must avoid `$'...'` or any quoting that interprets escape sequences.
- Quoting must preserve byte-equivalence with the original args; no quoting strategy may transform backslashes or escape sequences.
- Completion must also set `compopt -o nospace` to avoid an extra space breaking the quoted argument.
- Completion must treat `pick` output as a single-line payload; if output contains newline or control characters, treat as cancel/failure and return no completion.
- Quote strategy must cover single quotes in args without using escape-interpreting forms; never emit an unterminated or partially escaped string.
- Completion compatibility mode (MUST):
  - Provide a mode switch `SSHTAB_COMPLETION_MODE=always|fallback`.
  - In `fallback` mode (default), if `sshtab pick` cancels/fails or history is empty, the completion must return success with empty `COMPREPLY`, not altering the command line, allowing native ssh completion to proceed.
- Hooks only active for interactive shells; use guard to avoid recursion.
- `PROMPT_COMMAND` must run sshtab post-hook first (prepend). The first line of the post-hook must capture the previous exit code: `ec=$?`.
- Reason: user `PROMPT_COMMAND` entries can overwrite `$?`, so recording must capture it before any other command runs.
- `PROMPT_COMMAND` composition must be idempotent; reinstalling must not insert duplicate hooks, and sshtab hook must remain first.
- Install strategy must preserve existing `PROMPT_COMMAND`:
  - If `PROMPT_COMMAND` is an array, unshift the sshtab post-hook function to index 0 if not present.
  - If it is a string, prefix with `__sshtab_post_hook;` (or equivalent) only if not already present.
- DEBUG trap must not clobber existing traps; see "DEBUG Trap Compatibility" below.
- Snippet is idempotent with begin/end markers.

## DEBUG Trap Compatibility (Pre-Hook)
Strategy A (chain, required for v1): read existing trap with `trap -p DEBUG`, then install a new DEBUG trap that runs sshtab pre-hook first and then the original trap command.
Implementation notes:
- If no existing trap, set DEBUG to only run `__sshtab_pre_hook`.
- If an existing trap is found, store its command string and splice it after `__sshtab_pre_hook` when installing the new trap.
- The chaining must not lose or reorder the existing trap logic.
- Trap chaining must not use `eval` or any secondary interpretation of the trap string.
- The original trap must be preserved and restored using a safe string mechanism; if the trap format is complex or cannot be safely parsed/restored, sshtab must disable its DEBUG trap and emit a warning (install-time or doctor/self-check).
Practical chaining rules (MUST):
- Only enable chaining when the existing trap is safe and unambiguous (single-line, no embedded newlines, no complex nested quotes/escapes that cannot be preserved verbatim).
- If these conditions are not met, MUST fall back: do not install sshtab DEBUG trap; keep only the PROMPT_COMMAND post-hook (recording is disabled for this session).
- MUST NOT attempt to force chaining via `eval`, `source`, or any secondary parsing of the trap string; if safe chaining is not possible, fallback is mandatory.
Guard rules:
- Any sshtab-invoked helper (`record`, `pick`, `exec`) must not trigger pre-hook.
- The ssh() proxy must set `SSHTAB_GUARD=1` immediately on entry and clear it before return.
- The pre-hook must return immediately when `SSHTAB_GUARD` is set.
- The pre-hook must only capture user-entered `ssh` commands and must ignore internal calls such as `sshtab ...` or `command "$SSHTAB_REAL_SSH" ...` when guard is not set.
- The pre-hook MUST only run in interactive shells (`[[ $- == *i* ]]`).
- The pre-hook MUST match the command name token `ssh` strictly (e.g., regex `^ssh([[:space:]]|$)`), not a substring.
Pre-hook disabled behavior (MUST):
- If pre-hook is disabled (e.g., `SSHTAB_PREHOOK_ENABLED=0`), post-hook MUST return immediately and MUST NOT call `sshtab record`.
- Warning/notice about disabled pre-hook MUST be shown at most once per shell session to avoid prompt spam.

## Record Boundary Rules
- Pre-hook stores only the raw command string for user-entered `ssh ...`; it must not store commands issued by sshtab itself.
- Post-hook must call `sshtab record` with guard enabled to avoid recursive capture.
- `record` must re-validate that the normalized command name is exactly `ssh`; if not, it must ignore the entry.

## Allowed / Forbidden Behaviors
Allowed:
- Read and write only to XDG data dir and user-local paths.
- Use `/dev/tty` for TUI I/O.
- Fail fast on invalid input and return non-zero.

Forbidden:
- Running `/bin/sh`, `eval`, or executing arbitrary shell strings.
- Overwriting `PROMPT_COMMAND` or putting sshtab post-hook after existing commands.
- Replacing a user DEBUG trap without chaining or explicit degrade behavior.
- Writing to stdout from `pick` except the final selection line.
- Recording commands that are not `ssh` or have non-zero exit codes.
- Hardcoding `/usr/bin/ssh` or assuming a fixed ssh path.
- Global key binding or changes outside the marked bashrc block.

## Development Order
1) Core utilities: XDG path, base64 encode/decode, file lock helpers.
2) History read/append + dedupe logic (unit tests).
3) Normalization for `record` and args extraction for `pick`.
4) Tokenizer + `exec` safety checks (unit tests).
5) CLI dispatch + `record` and `list` commands.
6) TUI `pick` with filter + /dev/tty handling.
7) Bash snippet (proxy, hooks, completion).
8) Install/uninstall scripts with idempotent markers.
9) README + GitHub Actions release workflow.

## Acceptance Criteria
- `ssh<Tab>` or `ssh <Tab>` opens TUI with recent commands.
- Selecting a command inserts `ssh '<args>'` on the prompt.
- Running that command uses the resolved real ssh path and succeeds.
- Only exit code 0 SSH commands are recorded.
- Recents are unique and sorted by most recent.
- Normal SSH completions remain unaffected outside the exact trigger case.
- Uninstall removes bashrc block and binary; data removal is optional.
- If DEBUG trap chaining is unsafe, sshtab disables pre-hook and warns instead of risking user trap breakage.
- In `fallback` completion mode, pick cancel/failure/empty history yields native ssh completion.
- When pre-hook is disabled, no new entries are written to `history.log`.
- Typing `ssh a<Tab>` must use native ssh host/known_hosts completion.

## Open Questions / Assumptions
- `record` matches only `ssh` as the command name (no `sudo ssh` capture).
- `exec` rejects shell metacharacters even if ssh could accept them.
- Completion UI uses /dev/tty and ANSI codes; no external dependencies.
