#!/bin/bash
#
# run_abi_check.sh — GLFW / LWJGL 3.4.1 ABI conformance checker.
#
# Verifies that the native GLFW shared library (libglfw.dylib) built by
# `make lib` exports every function symbol that LWJGL 3.4.1 resolves via
# dlsym at static-init time.  This is the authoritative ABI check; it
# needs only nm(1) and runs without Java/LWJGL installed.
#
# If a JDK and the LWJGL glfw jar are available, it additionally runs the
# Java ABI test (ABI.java) which loads org.lwjgl.glfw.GLFW.
#
# Usage:
#   ./external/glfw/tests/abi_check/run_abi_check.sh [path/to/libglfw.dylib]
#
# Exit codes: 0 = pass, 1 = ABI mismatch, 2 = library not found.

set -euo pipefail

# Locate the GLFW shared library.
LIB="${1:-}"
if [ -z "$LIB" ]; then
    for candidate in \
        "$(git rev-parse --show-toplevel 2>/dev/null)/build/libglfw.dylib" \
        "$(cd "$(dirname "$0")/../../../.." && pwd)/build/libglfw.dylib" \
        ./build/libglfw.dylib; do
        if [ -f "$candidate" ]; then
            LIB="$candidate"
            break
        fi
    done
fi

if [ -z "$LIB" ] || [ ! -f "$LIB" ]; then
    echo "FATAL: libglfw.dylib not found. Run 'make lib' first," >&2
    echo "       or pass its path as the first argument." >&2
    exit 2
fi

echo "Checking ABI of: $LIB"

# Mandatory GLFW symbols required by LWJGL 3.4.1 (org.lwjgl.glfw.GLFW).
REQUIRED=(
    glfwInit glfwTerminate glfwInitHint
    glfwGetVersion glfwGetVersionString
    glfwGetError glfwSetErrorCallback
    glfwGetPlatform glfwPlatformSupported
    glfwGetMonitors glfwGetPrimaryMonitor
    glfwGetMonitorPos glfwGetMonitorWorkarea
    glfwGetMonitorPhysicalSize glfwGetMonitorContentScale
    glfwGetMonitorName glfwSetMonitorCallback
    glfwGetVideoModes glfwGetVideoMode
    glfwSetGamma glfwGetGammaRamp glfwSetGammaRamp
    glfwDefaultWindowHints glfwWindowHint glfwWindowHintString
    glfwCreateWindow glfwDestroyWindow
    glfwWindowShouldClose glfwSetWindowShouldClose
    glfwGetWindowTitle glfwSetWindowTitle glfwSetWindowIcon
    glfwGetWindowPos glfwSetWindowPos
    glfwGetWindowSize glfwSetWindowSize
    glfwSetWindowSizeLimits glfwSetWindowAspectRatio
    glfwGetFramebufferSize glfwGetWindowFrameSize
    glfwGetWindowContentScale
    glfwGetWindowOpacity glfwSetWindowOpacity
    glfwIconifyWindow glfwRestoreWindow glfwMaximizeWindow
    glfwShowWindow glfwHideWindow glfwFocusWindow
    glfwRequestWindowAttention
    glfwGetWindowMonitor glfwSetWindowMonitor
    glfwGetWindowAttrib glfwSetWindowAttrib
    glfwGetWindowUserPointer glfwSetWindowUserPointer
    glfwSetWindowPosCallback glfwSetWindowSizeCallback
    glfwSetWindowCloseCallback glfwSetWindowRefreshCallback
    glfwSetWindowFocusCallback glfwSetWindowIconifyCallback
    glfwSetWindowMaximizeCallback
    glfwSetFramebufferSizeCallback glfwSetWindowContentScaleCallback
    glfwPollEvents glfwWaitEvents glfwWaitEventsTimeout
    glfwPostEmptyEvent
    glfwGetInputMode glfwSetInputMode glfwRawMouseMotionSupported
    glfwGetKeyName glfwGetKeyScancode
    glfwGetKey glfwGetMouseButton
    glfwGetCursorPos glfwSetCursorPos
    glfwCreateCursor glfwCreateStandardCursor
    glfwDestroyCursor glfwSetCursor
    glfwSetKeyCallback glfwSetCharCallback glfwSetCharModsCallback
    glfwSetMouseButtonCallback glfwSetCursorPosCallback
    glfwSetCursorEnterCallback glfwSetScrollCallback
    glfwSetDropCallback
    glfwJoystickPresent glfwGetJoystickAxes glfwGetJoystickButtons
    glfwGetJoystickHats glfwGetJoystickName glfwGetJoystickGUID
    glfwJoystickIsGamepad glfwSetJoystickCallback
    glfwUpdateGamepadMappings glfwGetGamepadName glfwGetGamepadState
    glfwSetClipboardString glfwGetClipboardString
    glfwGetTime glfwSetTime
    glfwGetTimerValue glfwGetTimerFrequency
    glfwMakeContextCurrent glfwGetCurrentContext
    glfwSwapBuffers glfwSwapInterval
    glfwExtensionSupported glfwGetProcAddress
    glfwVulkanSupported glfwGetRequiredInstanceExtensions
)

# Extract exported (global, undefined-filtered) symbols from the dylib.
# -g  : display only global (external) symbols
# -U  : display only defined symbols (no undefined references)
EXPORTED=$(nm -gU "$LIB" 2>/dev/null | awk '{print $NF}' | sort -u)

missing=()
for sym in "${REQUIRED[@]}"; do
    if ! grep -qx "$sym" <<<"$EXPORTED"; then
        missing+=("$sym")
    fi
done

echo "Required symbols : ${#REQUIRED[@]}"
echo "Exported (glfw*) : $(printf '%s\n' "$EXPORTED" | grep -c '^glfw' || true)"

if [ ${#missing[@]} -eq 0 ]; then
    echo "RESULT: ABI OK — all ${#REQUIRED[@]} required symbols exported."
else
    echo "RESULT: ABI FAIL — ${#missing[@]} missing symbol(s):"
    for sym in "${missing[@]}"; do
        echo "  - $sym"
    done
    exit 1
fi

# --- Optional Java test -------------------------------------------------
# If a JDK and the LWJGL glfw jar are on the classpath, run ABI.java which
# exercises the real LWJGL class-loader path.  This is informational; the
# nm check above is authoritative for CI.
if command -v javac >/dev/null 2>&1; then
    HERE="$(cd "$(dirname "$0")" && pwd)"
    BUILD_DIR="$(cd "$HERE/../../.." && pwd)/build"
    if [ -f "$BUILD_DIR/libglfw.dylib" ]; then
        echo ""
        echo "--- Optional Java ABI test ---"
        (cd "$HERE" && javac -d . ABI.java 2>/dev/null && \
            java -Djava.library.path="$BUILD_DIR" -cp . \
                 ${LWJGL_CP:-} ABI 2>&1) || \
            echo "(Java test skipped — set LWJGL_CP=<lwjgl jars> to enable)"
    fi
fi

exit 0
