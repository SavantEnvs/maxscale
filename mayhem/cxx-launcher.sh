#!/usr/bin/env bash
# CMAKE_{C,CXX}_COMPILER_LAUNCHER wrapper: upstream's pp_sqlite.cc declares an initialized
# variable-length array, which GCC accepts but clang rejects as a hard error. Compile a copy with
# that one declaration split into declaration + assignment (same line count, #line keeps the
# original file name in debug info) instead of editing the upstream file.
set -euo pipefail
out=()
for a in "$@"; do
  case "$a" in
    */parser_plugin/pp_sqlite/pp_sqlite.cc)
      dst="${MAYHEM_COMPAT_DIR:?}/pp_sqlite.cc"
      mkdir -p "$MAYHEM_COMPAT_DIR"
      { echo "#line 1 \"$a\""
        sed 's|char database\[nDatabase + 1\] = "";|char database[nDatabase + 1]; database[0] = 0;|' "$a"
      } > "$dst.$$"
      mv "$dst.$$" "$dst"
      out+=("-I$(dirname "$a")")
      a="$dst" ;;
  esac
  out+=("$a")
done
exec "${out[@]}"
