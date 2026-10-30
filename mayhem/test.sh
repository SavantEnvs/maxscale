#!/usr/bin/env bash
#
# mayhem/test.sh — RUNS the known-answer oracle (mayhem/oracle.cc) built by mayhem/build.sh: it classifies
# fixed SQL statements through MaxScale's pp_sqlite parser plugin and maxsimd canonicalizer and compares
# operation/type mask/tables/fields/canonical form/multi-statement flag against hard-coded expectations.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}


ORACLE="$SRC/build/test/oracle"
[ -x "$ORACLE" ] || { echo "missing $ORACLE — mayhem/build.sh must build it" >&2; emit_ctrf "maxscale-oracle" 0 1 0; exit 1; }

export PP_SQLITE_LIBDIR="$SRC/build/test/server/modules/parser_plugin/pp_sqlite"
out="$("$ORACLE" 2>&1)"; rc=$?
printf '%s\n' "$out" | grep -E '^(PASS|FAIL)' || true
passed=$(printf '%s\n' "$out" | grep -c '^PASS ' || true)
failed=$(printf '%s\n' "$out" | grep -c '^FAIL ' || true)
# A run that exits non-zero without a FAIL line (crash, plugin failed to load) is a failure too.
if [ "$rc" -ne 0 ] && [ "$failed" -eq 0 ]; then failed=1; fi
# An oracle that checks nothing (e.g. neutered to exit(0)) must not pass.
if [ "$passed" -eq 0 ]; then failed=$((failed + 1)); fi
emit_ctrf "maxscale-oracle" "$passed" "$failed" 0
