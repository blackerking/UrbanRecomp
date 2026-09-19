#!/usr/bin/env bash
# Regen pipeline driver for UrbanRecomp.
#
# Regenerates banks from recomp/*.cfg and the verified ROM, then syncs
# recomp/funcs.h.
#
# Flags:
#   --no-tests             skip the framework test suite (default: run it).
#   --strict-idempotent    regenerate into a temporary directory and require
#                          byte-identical output.
#   -h | --help             this message.
#
# Run from anywhere - paths resolve relative to this script's location.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

RUN_TESTS=1
STRICT_IDEMPOTENT=0
for arg in "$@"; do
  case "$arg" in
    --no-tests) RUN_TESTS=0 ;;
    --strict-idempotent) STRICT_IDEMPOTENT=1 ;;
    -h|--help)  sed -n '2,/^set -euo/p' "$0" | sed -n '/^# /p' | sed 's/^# //'; exit 0 ;;
    *) echo "regen.sh: unknown argument: $arg (try --help)" >&2; exit 2 ;;
  esac
done

cd "$ROOT"

SNESRECOMP_ROOT="${SNESRECOMP_ROOT:-snesrecomp}"
TESTS="$SNESRECOMP_ROOT/tests/run_tests.py"
ROM="${ROM:-}"   # the US ROM; found by its contents when unset
CFG_DIR="recomp"
OUT_DIR="src/gen"
FUNCS_H="recomp/funcs.h"

if [ ! -f "$SNESRECOMP_ROOT/tools/v2_emit.py" ]; then
  echo "regen.sh: snesrecomp is not initialized; run 'bash tools/bootstrap.sh' first." >&2
  exit 1
fi

# Python interpreter: prefer python3 (macOS / most Linux have no bare `python`).
PYTHON="${PYTHON:-$(command -v python3 || command -v python || true)}"
if [ -z "$PYTHON" ]; then
  echo "regen.sh: no python3/python interpreter found on PATH" >&2
  exit 1
fi

if [ -z "$ROM" ]; then
  ROM="$("$PYTHON" tools/find_rom.py us)" || exit 1
fi
if [ ! -f "$ROM" ]; then
  echo "regen.sh: $ROM not found - pass ROM=<your US ROM>, or put it (any file name) in the repository root." >&2
  exit 1
fi

step() { echo; echo "=== $* ==="; }

ANALYSIS_BACKEND="${SNESRECOMP_ANALYSIS_BACKEND:-native}"
case "$ANALYSIS_BACKEND" in
  native|python|auto) ;;
  *) echo "regen.sh: invalid SNESRECOMP_ANALYSIS_BACKEND: $ANALYSIS_BACKEND" >&2; exit 2 ;;
esac

if [ "$ANALYSIS_BACKEND" = native ]; then
  step "Building native analyzer"
  "$PYTHON" "$SNESRECOMP_ROOT/tools/build_native_analyzer.py"
fi

step "Regenerating banks"
# LLE-first emitter (snesrecomp/docs/LLE_FIRST_ANALYSIS.md): --cfg-roots seeds
# the analysis closure from every declared `func` (currently none -- only
# auto_vectors) union'd with the architectural RESET/NMI/IRQ vectors. Whatever
# the analyzer cannot yet prove AOT-eligible keeps running correctly on the
# LLE interpreter tier; that tier is the correctness baseline, not this step.
"$PYTHON" "$SNESRECOMP_ROOT/tools/v2_emit.py" --rom "$ROM" \
    --cfg-dir "$CFG_DIR" --out-dir "$OUT_DIR" --cfg-roots \
    --analysis-backend "$ANALYSIS_BACKEND"

step "Syncing funcs.h"
"$PYTHON" "$SNESRECOMP_ROOT/tools/v2_sync_funcs_h.py" --cfg-dir "$CFG_DIR" \
    --out "$FUNCS_H"

if [ "$STRICT_IDEMPOTENT" -eq 1 ]; then
  step "Checking idempotency"
  tmp_gen="$(mktemp -d)"
  "$PYTHON" "$SNESRECOMP_ROOT/tools/v2_emit.py" --rom "$ROM" \
      --cfg-dir "$CFG_DIR" --out-dir "$tmp_gen" --cfg-roots \
      --analysis-backend "$ANALYSIS_BACKEND"
  "$PYTHON" "$SNESRECOMP_ROOT/tools/v2_compare_output.py" \
      --expected "$OUT_DIR" --actual "$tmp_gen"
  rm -rf "$tmp_gen"
fi

if [ "$RUN_TESTS" -eq 1 ]; then
  step "Framework tests"
  "$PYTHON" "$TESTS"
fi

step "Done"
