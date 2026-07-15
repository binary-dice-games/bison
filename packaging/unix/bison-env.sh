#!/usr/bin/env bash
# Source this file to use this extracted bison release from the current
# shell session only -- no files are modified:
#
#   source ./bison-env.sh
#
# For a setup that persists across new shells, run ./install.sh once
# instead (it appends an equivalent block to your shell rc file).
#
# Linux's dynamic linker (unlike Windows') does not consult PATH when
# resolving shared libraries, so `bison-cli` being on PATH is not enough on
# its own for a *separate* program (e.g. your own C/C++/Python tool linking
# bison_abi) to find libbison_abi.so -- LD_LIBRARY_PATH (or BISON_LIB, which
# bindings/python/bison reads directly) is required too.

if [ -n "${BASH_SOURCE:-}" ]; then
  _bison_src="${BASH_SOURCE[0]}"
elif [ -n "${ZSH_VERSION:-}" ]; then
  _bison_src="${(%):-%N}"
else
  _bison_src="$0"
fi
_bison_root="$(cd "$(dirname "$_bison_src")" && pwd)"
unset _bison_src

export PATH="${_bison_root}/bin:${PATH}"
export LD_LIBRARY_PATH="${_bison_root}/bin:${LD_LIBRARY_PATH:-}"
export BISON_LIB="${_bison_root}/bin/libbison_abi.so"
unset _bison_root

echo "bison is on PATH for this shell session (try: bison-cli --help)"
