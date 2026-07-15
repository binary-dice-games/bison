#!/usr/bin/env bash
# Persistently adds this extracted bison release to your PATH (and
# LD_LIBRARY_PATH / BISON_LIB) by appending a marked block to your shell rc
# file (~/.bashrc or ~/.zshrc, detected from $SHELL). Idempotent -- running
# it again is a no-op if the block is already present.
#
#   ./install.sh
#
# For a one-off session instead of a persistent change, use
# `source ./bison-env.sh` and skip this script entirely.
#
# To undo: ./install.sh --uninstall  (or manually delete the block between
# the "bison release PATH setup" markers in your rc file).

set -euo pipefail

bison_root="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

case "$(basename "${SHELL:-bash}")" in
  zsh)  rc_file="${HOME}/.zshrc" ;;
  *)    rc_file="${HOME}/.bashrc" ;;
esac

# Constant, path-free markers -- the installed path is recorded on its own
# line *inside* the block (informational only), never inside the marker
# text used for matching, so neither marker can collide with a delimiter
# character (sed/awk pattern-matching a literal filesystem path is fragile:
# paths routinely contain '/', and a marker containing '#' can't itself be
# used as a '#'-delimited sed address).
marker_begin="# >>> bison release PATH setup >>>"
marker_end="# <<< bison release PATH setup <<<"

if [ "${1:-}" = "--uninstall" ]; then
  if [ -f "$rc_file" ] && grep -qF "$marker_begin" "$rc_file"; then
    awk -v b="$marker_begin" -v e="$marker_end" '
      $0 == b { skip = 1 }
      !skip { print }
      $0 == e { skip = 0 }
    ' "$rc_file" > "${rc_file}.tmp" && mv "${rc_file}.tmp" "$rc_file"
    echo "Removed bison PATH setup from $rc_file."
  else
    echo "No bison PATH setup found in $rc_file."
  fi
  exit 0
fi

if [ -f "$rc_file" ] && grep -qF "$marker_begin" "$rc_file"; then
  echo "bison PATH setup already present in $rc_file -- nothing to do."
  exit 0
fi

{
  printf '\n%s\n' "$marker_begin"
  printf '# installed from: %s\n' "$bison_root"
  printf 'export PATH="%s/bin:$PATH"\n' "$bison_root"
  printf 'export LD_LIBRARY_PATH="%s/bin:${LD_LIBRARY_PATH:-}"\n' "$bison_root"
  printf 'export BISON_LIB="%s/bin/libbison_abi.so"\n' "$bison_root"
  printf '%s\n' "$marker_end"
} >> "$rc_file"

echo "Added bison to PATH in $rc_file."
echo "Restart your shell, or run: source $rc_file"
echo "To undo: $0 --uninstall"
