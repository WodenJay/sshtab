#!/usr/bin/env bash
set -euo pipefail

REPO_OWNER="WodenJay"
REPO_NAME="sshtab"

if [[ -n ${SSHTAB_REPO:-} ]]; then
  if [[ ${SSHTAB_REPO} != */* ]]; then
    echo "sshtab: SSHTAB_REPO must be in OWNER/REPO format" >&2
    exit 1
  fi
  REPO_OWNER=${SSHTAB_REPO%%/*}
  REPO_NAME=${SSHTAB_REPO##*/}
fi

if [[ $(uname -s) != "Linux" ]]; then
  echo "sshtab: unsupported OS (Linux only)" >&2
  exit 1
fi

if [[ $(uname -m) != "x86_64" ]]; then
  echo "sshtab: unsupported architecture (x86_64 only)" >&2
  exit 1
fi

TMP_DIR=$(mktemp -d)
cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

API_URL="https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}/releases/latest"
json=$(curl -fsSL "${API_URL}")

bin_url=""
sha_url=""
while IFS= read -r url; do
  case "${url}" in
    *"/sshtab-linux-x86_64") bin_url="${url}" ;;
    *"/sha256sums.txt") sha_url="${url}" ;;
  esac
done < <(printf '%s' "${json}" | sed -n 's/.*"browser_download_url":[[:space:]]*"\([^"]*\)".*/\1/p')

if [[ -z ${bin_url} || -z ${sha_url} ]]; then
  echo "sshtab: release assets not found" >&2
  exit 1
fi

curl -fL "${bin_url}" -o "${TMP_DIR}/sshtab-linux-x86_64"
curl -fL "${sha_url}" -o "${TMP_DIR}/sha256sums.txt"

hash=$(awk '$2=="sshtab-linux-x86_64" {print $1}' "${TMP_DIR}/sha256sums.txt")
if [[ -z ${hash} ]]; then
  echo "sshtab: sha256 entry not found" >&2
  exit 1
fi

echo "${hash}  ${TMP_DIR}/sshtab-linux-x86_64" | sha256sum -c - >/dev/null 2>&1 || {
  echo "sshtab: sha256 verification failed" >&2
  exit 1
}

BIN_DIR="${HOME}/.local/bin"
mkdir -p "${BIN_DIR}"
cp "${TMP_DIR}/sshtab-linux-x86_64" "${BIN_DIR}/sshtab"
chmod 0755 "${BIN_DIR}/sshtab"

smoke_out=$("${BIN_DIR}/sshtab" list --limit 1 2>&1 || true)
if echo "${smoke_out}" | grep -q 'GLIBC_\\|GLIBCXX_'; then
  echo "sshtab: binary is not compatible with this system (${smoke_out})" >&2
  exit 1
fi

SNIPPET_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/sshtab"
mkdir -p "${SNIPPET_DIR}"
SNIPPET_PATH="${SNIPPET_DIR}/sshtab.bash"
SNIPPET_URL="https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/main/bash/sshtab.bash"
curl -fsSL "${SNIPPET_URL}" -o "${SNIPPET_PATH}"
chmod 0644 "${SNIPPET_PATH}"

BASHRC="${HOME}/.bashrc"
MARK_BEGIN="# >>> sshtab begin >>>"
MARK_END="# <<< sshtab end <<<"

if [[ ! -f "${BASHRC}" ]]; then
  touch "${BASHRC}"
fi

if ! grep -q "^${MARK_BEGIN}$" "${BASHRC}"; then
  {
    echo "${MARK_BEGIN}"
    echo "[ -f \"${SNIPPET_PATH}\" ] && source \"${SNIPPET_PATH}\""
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

echo "sshtab installed. Run: source ~/.bashrc (or restart your shell)"
echo "Type ssh <Tab> to use sshtab."
