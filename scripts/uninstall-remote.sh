#!/usr/bin/env bash
set -euo pipefail

PURGE=0
if [[ ${1:-} == "--purge" ]]; then
  PURGE=1
fi

BIN_DIR="${HOME}/.local/bin"
BIN_DEST="${BIN_DIR}/sshtab"
DATA_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/sshtab"
SNIPPET_DEST="${DATA_DIR}/sshtab.bash"
BASHRC="${HOME}/.bashrc"

MARK_BEGIN="# >>> sshtab begin >>>"
MARK_END="# <<< sshtab end <<<"

if [[ -f "${BASHRC}" ]]; then
  tmp=$(mktemp)
  awk -v begin="${MARK_BEGIN}" -v end="${MARK_END}" '
    $0 == begin {in=1; next}
    $0 == end {in=0; next}
    in!=1 {print}
  ' "${BASHRC}" > "${tmp}"
  mv "${tmp}" "${BASHRC}"
fi

rm -f "${BIN_DEST}"
rm -f "${SNIPPET_DEST}"

if [[ ${PURGE} -eq 1 ]]; then
  rm -rf "${DATA_DIR}"
fi

echo "sshtab removed. Run: source ~/.bashrc (or restart your shell)"
