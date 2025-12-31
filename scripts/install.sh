#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)

BIN_SRC=${SSHTAB_BIN:-"${REPO_DIR}/sshtab"}
SNIPPET_SRC="${REPO_DIR}/bash/sshtab.bash"

if [[ ! -x "${BIN_SRC}" ]]; then
  echo "sshtab: binary not found or not executable at ${BIN_SRC}" >&2
  echo "Build it with: g++ -std=c++17 -O2 -Wall -Wextra -pedantic src/*.cpp -o sshtab" >&2
  exit 1
fi

if [[ ! -f "${SNIPPET_SRC}" ]]; then
  echo "sshtab: snippet not found at ${SNIPPET_SRC}" >&2
  exit 1
fi

BIN_DIR="${HOME}/.local/bin"
mkdir -p "${BIN_DIR}"
cp "${BIN_SRC}" "${BIN_DIR}/sshtab"
chmod 0755 "${BIN_DIR}/sshtab"

DATA_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/sshtab"
mkdir -p "${DATA_DIR}"
SNIPPET_DEST="${DATA_DIR}/sshtab.bash"
cp "${SNIPPET_SRC}" "${SNIPPET_DEST}"
chmod 0644 "${SNIPPET_DEST}"

BASHRC="${HOME}/.bashrc"
MARK_BEGIN="# >>> sshtab begin >>>"
MARK_END="# <<< sshtab end <<<"

if [[ ! -f "${BASHRC}" ]]; then
  touch "${BASHRC}"
fi

if ! grep -q "^${MARK_BEGIN}$" "${BASHRC}"; then
  {
    echo "${MARK_BEGIN}"
    echo "[ -f \"${SNIPPET_DEST}\" ] && source \"${SNIPPET_DEST}\""
    echo "${MARK_END}"
  } >> "${BASHRC}"
fi

case ":${PATH}:" in
  *":${BIN_DIR}:"*)
    ;;
  *)
    echo "sshtab: ${BIN_DIR} is not in PATH. Add this to your shell config:" >&2
    echo "  export PATH=\"${BIN_DIR}:\$PATH\"" >&2
    ;;
 esac

echo "sshtab installed. Restart your shell or run: source ~/.bashrc"
