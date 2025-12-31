# sshtab Bash integration snippet

__sshtab_warn_once() {
  local flag="$1"
  shift
  if [[ -z ${!flag} ]]; then
    printf '%s\n' "$*" >&2
    printf -v "$flag" '1'
  fi
}

__sshtab_quote_single() {
  local s="$1"
  local out="'"
  while [[ $s == *"'"* ]]; do
    out+="${s%%\'*}"
    out+="'\\''"
    s=${s#*\'}
  done
  out+="$s'"
  printf '%s' "$out"
}

__sshtab_detect_prev_completion() {
  local spec
  spec=$(complete -p ssh 2>/dev/null) || return
  if [[ $spec =~ -F[[:space:]]+([_a-zA-Z0-9]+) ]]; then
    SSHTAB_PREV_COMPLETION_FUNC="${BASH_REMATCH[1]}"
  fi
}

__sshtab_call_prev_completion() {
  if [[ -n ${SSHTAB_PREV_COMPLETION_FUNC:-} ]] && [[ $(type -t "${SSHTAB_PREV_COMPLETION_FUNC}") == "function" ]]; then
    "${SSHTAB_PREV_COMPLETION_FUNC}"
    return 0
  fi
  return 1
}

SSHTAB_REAL_SSH=""
SSHTAB_SSH_PROXY_ENABLED=0
SSHTAB_PREHOOK_ENABLED=1
SSHTAB_COMPLETION_MODE=${SSHTAB_COMPLETION_MODE:-fallback}
SSHTAB_LIMIT=${SSHTAB_LIMIT:-50}

SSHTAB_REAL_SSH=$(type -P ssh 2>/dev/null)
if [[ -n ${SSHTAB_REAL_SSH} ]]; then
  SSHTAB_SSH_PROXY_ENABLED=1
else
  SSHTAB_SSH_PROXY_ENABLED=0
  __sshtab_warn_once SSHTAB_WARNED_SSH_PATH "sshtab: cannot resolve real ssh path; proxy disabled"
fi

if [[ ${SSHTAB_SSH_PROXY_ENABLED} -eq 1 ]]; then
  ssh() {
    local prev_guard=${SSHTAB_GUARD:-}
    SSHTAB_GUARD=1
    if [[ $# -eq 1 && "$1" == *" "* ]]; then
      sshtab exec "$1"
      local rc=$?
      if [[ -n $prev_guard ]]; then
        SSHTAB_GUARD=$prev_guard
      else
        unset SSHTAB_GUARD
      fi
      return $rc
    fi
    command "${SSHTAB_REAL_SSH}" "$@"
    local rc=$?
    if [[ -n $prev_guard ]]; then
      SSHTAB_GUARD=$prev_guard
    else
      unset SSHTAB_GUARD
    fi
    return $rc
  }
fi

__sshtab_pre_hook() {
  [[ $- == *i* ]] || return
  [[ ${SSHTAB_PREHOOK_ENABLED:-1} -eq 1 ]] || return
  [[ -z ${SSHTAB_GUARD:-} ]] || return

  local cmd="$BASH_COMMAND"
  if [[ $cmd =~ ^ssh([[:space:]]|$) ]]; then
    SSHTAB_PENDING_RAW="$cmd"
  fi
}

__sshtab_post_hook() {
  local ec=$?

  if [[ ${SSHTAB_PREHOOK_ENABLED:-1} -eq 0 ]]; then
    __sshtab_warn_once SSHTAB_WARNED_PREHOOK_DISABLED "sshtab: pre-hook disabled; recording skipped"
    return
  fi

  if [[ -z ${SSHTAB_PENDING_RAW:-} ]]; then
    return
  fi

  local raw="$SSHTAB_PENDING_RAW"
  SSHTAB_PENDING_RAW=""

  if [[ $ec -ne 0 ]]; then
    return
  fi

  local prev_guard=${SSHTAB_GUARD:-}
  SSHTAB_GUARD=1
  sshtab record --exit-code "$ec" --raw "$raw" >/dev/null 2>&1
  if [[ -n $prev_guard ]]; then
    SSHTAB_GUARD=$prev_guard
  else
    unset SSHTAB_GUARD
  fi
}

__sshtab_complete() {
  local cur="${COMP_WORDS[COMP_CWORD]}"
  local line_prefix="${COMP_LINE:0:COMP_POINT}"

  if [[ ${COMP_CWORD:-0} -ne 1 || -n $cur || ! $line_prefix =~ ^[[:space:]]*ssh[[:space:]]*$ ]]; then
    COMPREPLY=()
    __sshtab_call_prev_completion
    return 0
  fi

  local args
  args=$(sshtab pick --limit "${SSHTAB_LIMIT}" --non-interactive --select 0 2>/dev/null) || {
    COMPREPLY=()
    if [[ ${SSHTAB_COMPLETION_MODE} == "fallback" ]]; then
      __sshtab_call_prev_completion
    fi
    return 0
  }

  if [[ -z $args || $args =~ [[:cntrl:]] ]]; then
    COMPREPLY=()
    if [[ ${SSHTAB_COMPLETION_MODE} == "fallback" ]]; then
      __sshtab_call_prev_completion
    fi
    return 0
  fi

  local quoted
  quoted=$(__sshtab_quote_single "$args")
  COMPREPLY=("$quoted")
  compopt -o nospace 2>/dev/null
  return 0
}

if [[ $- == *i* ]]; then
  if [[ -n $(trap -p DEBUG) ]]; then
    SSHTAB_PREHOOK_ENABLED=0
    __sshtab_warn_once SSHTAB_WARNED_PREHOOK_DISABLED "sshtab: DEBUG trap already set; pre-hook disabled"
  else
    SSHTAB_PREHOOK_ENABLED=1
    trap '__sshtab_pre_hook' DEBUG
  fi

  if [[ $(declare -p PROMPT_COMMAND 2>/dev/null) == declare\ -a* ]]; then
    _sshtab_has=0
    for _pc in "${PROMPT_COMMAND[@]}"; do
      if [[ $_pc == "__sshtab_post_hook" ]]; then
        _sshtab_has=1
        break
      fi
    done
    if [[ $_sshtab_has -eq 0 ]]; then
      PROMPT_COMMAND=(__sshtab_post_hook "${PROMPT_COMMAND[@]}")
    fi
    unset _pc _sshtab_has
  else
    if [[ ${PROMPT_COMMAND:-} != __sshtab_post_hook* ]]; then
      _pc="${PROMPT_COMMAND:-}"
      _pc=${_pc//__sshtab_post_hook; /}
      _pc=${_pc//__sshtab_post_hook;/}
      _pc=${_pc//__sshtab_post_hook /}
      _pc=${_pc//__sshtab_post_hook/}
      if [[ -n $_pc ]]; then
        PROMPT_COMMAND="__sshtab_post_hook;${_pc}"
      else
        PROMPT_COMMAND="__sshtab_post_hook"
      fi
      unset _pc
    fi
  fi

  __sshtab_spec=$(complete -p ssh 2>/dev/null)
  if [[ ${__sshtab_spec} != *"__sshtab_complete"* ]]; then
    __sshtab_detect_prev_completion
    complete -F __sshtab_complete ssh
  fi
  unset __sshtab_spec
fi
