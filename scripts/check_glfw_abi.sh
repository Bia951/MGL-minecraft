#!/usr/bin/env bash
#
# check_glfw_abi.sh — GLFW / LWJGL 3.4.1 ABI conformance checker (Spec R1 / Task 1).
#
# Verifies that the native GLFW shared library (libglfw.dylib) built by
# `make lib` exports every public GLFW function symbol declared in
# external/glfw/include/GLFW/glfw3.h (the 3.4 header).  LWJGL 3.4.1's
# `org.lwjgl.glfw.GLFW` resolves these symbols by name (dlsym) at static-init
# time; if any mandatory symbol is missing the JVM aborts with
# UnsatisfiedLinkError during GLFW class load, before Minecraft even starts.
#
# This script is the authoritative, environment-portable ABI check — it needs
# only nm(1) and runs without Java/LWJGL installed.  It is the verification
# gate for Spec R1 / Task 1 ("complete the GLFW 3.4 ABI").
#
# Usage:
#   scripts/check_glfw_abi.sh                     # auto-locate build/libglfw.dylib
#   scripts/check_glfw_abi.sh path/to/libglfw.dylib
#   scripts/check_glfw_abi.sh --header external/glfw/include/GLFW/glfw3.h
#   scripts/check_glfw_abi.sh --dylib build/libglfw.dylib
#
# Exit codes:
#   0  — ABI OK: every required symbol is exported.
#   1  — ABI FAIL: one or more required symbols missing.
#   2  — library or header not found / nm unavailable.
#
# Notes:
#   - On macOS, `nm` prepends a single underscore to C symbols (`_glfwInit`
#     for `glfwInit`).  This script strips the leading underscore before
#     comparison so the same REQUIRED list works on all platforms.
#   - The REQUIRED list is derived from every `GLFWAPI ... glfwFoo(...)`
#     declaration in glfw3.h (124 functions in the 3.4 release).  This is a
#     strict superset of the symbols LWJGL 3.4.1 statically links, so passing
#     this check also satisfies LWJGL's mandatory-symbol bootstrap.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DEFAULT_DYLIB="$PROJECT_DIR/build/libglfw.dylib"
DEFAULT_HEADER="$PROJECT_DIR/external/glfw/include/GLFW/glfw3.h"

LIB=""
HEADER=""

while [ $# -gt 0 ]; do
  case "$1" in
    --dylib)  LIB="$2"; shift 2 ;;
    --header) HEADER="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *)
      # Positional argument: treat as the dylib path.
      LIB="$1"; shift ;;
  esac
done

[ -z "$LIB" ]    && LIB="$DEFAULT_DYLIB"
[ -z "$HEADER" ] && HEADER="$DEFAULT_HEADER"

if [ ! -f "$LIB" ]; then
  echo "FATAL: GLFW shared library not found at: $LIB" >&2
  echo "       Run 'make lib' first, or pass its path as the first argument." >&2
  exit 2
fi

if [ ! -f "$HEADER" ]; then
  echo "FATAL: glfw3.h not found at: $HEADER" >&2
  echo "       Pass --header <path> to override." >&2
  exit 2
fi

if ! command -v nm >/dev/null 2>&1; then
  echo "FATAL: nm(1) not found in PATH; cannot inspect symbols." >&2
  exit 2
fi

# --- Build the required-symbol list from glfw3.h -----------------------
#
# Extract every `GLFWAPI <type> glfwFoo(...)` declaration.  We grab the
# function name (the first glfw-prefixed identifier on each GLFWAPI line).
# This keeps the list in sync with the header automatically — no hand
# maintenance.
REQUIRED=$(grep -E '^GLFWAPI' "$HEADER" \
           | grep -oE 'glfw[A-Za-z0-9_]+' \
           | sort -u)

if [ -z "$REQUIRED" ]; then
  echo "FATAL: no GLFWAPI declarations found in $HEADER" >&2
  exit 2
fi

required_count=$(printf '%s\n' "$REQUIRED" | wc -l | tr -d ' ')

# --- Extract exported symbols from the dylib ---------------------------
#
# `nm -gU`:
#   -g  display only global (external) symbols
#   -U  display only defined symbols (skip undefined references)
# On macOS, C symbols are emitted with a leading underscore; strip it so the
# names match the source/header identifiers.
#
# We also fall back to checking libmgl.dylib in case glfw symbols are
# re-exported from there rather than from libglfw.dylib (some MGL layouts
# do this).  The union is used for comparison.
EXPORTED=""
for candidate in "$LIB" "$PROJECT_DIR/build/libmgl.dylib"; do
  [ -f "$candidate" ] || continue
  EXPORTED="$EXPORTED
$(nm -gU "$candidate" 2>/dev/null | awk '{print $NF}' | sed 's/^_//' | grep '^glfw' || true)"
done
EXPORTED=$(printf '%s\n' "$EXPORTED" | grep -v '^$' | sort -u)

exported_count=$(printf '%s\n' "$EXPORTED" | wc -l | tr -d ' ')

# --- Compare -----------------------------------------------------------
missing=()
while IFS= read -r sym; do
  [ -z "$sym" ] && continue
  if ! printf '%s\n' "$EXPORTED" | grep -qx "$sym"; then
    missing+=("$sym")
  fi
done <<<"$REQUIRED"

echo "Checking ABI of : $LIB"
echo "Header          : $HEADER"
echo "Required symbols: $required_count"
echo "Exported (glfw*): $exported_count"

if [ ${#missing[@]} -eq 0 ]; then
  echo "RESULT: ABI OK — all $required_count required GLFW symbols are exported."
  exit 0
fi

echo "RESULT: ABI FAIL — ${#missing[@]} missing symbol(s):"
for sym in "${missing[@]}"; do
  echo "  - $sym"
done
exit 1
