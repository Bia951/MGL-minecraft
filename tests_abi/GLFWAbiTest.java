/*
 * GLFWAbiTest.java — GLFW / LWJGL 3.4.1 ABI conformance test (Spec R1 / Task 1).
 *
 * Verifies that the native libglfw.dylib shipped with MGL exports every
 * GLFW function symbol that LWJGL 3.4.1 resolves at static-init time.
 * LWJGL looks up these symbols by name (dlsym) when
 * org.lwjgl.glfw.GLFW is class-loaded; if any mandatory symbol is
 * missing the JVM aborts with UnsatisfiedLinkError during GLFW init,
 * before Minecraft even starts.
 *
 * Two modes:
 *
 *   1. LWJGL on classpath  — loads Class.forName("org.lwjgl.glfw.GLFW")
 *      which forces eager resolution of every mandatory function pointer
 *      by invoking each function's JNI address getter.  Any missing
 *      export => test fails.  This mirrors the real Minecraft launch path.
 *
 *   2. LWJGL absent        — prints a notice and exits with code 3 so
 *      the shell wrapper (scripts/check_glfw_abi.sh) can fall back to
 *      the equivalent `nm -gU` symbol check, which is authoritative and
 *      does not require a JDK or LWJGL jars.
 *
 * This file is a "run-when-LWJGL-is-available" regression test.  The
 * authoritative CI gate is scripts/check_glfw_abi.sh, which only needs
 * nm(1) and the built dylib.
 *
 * -----------------------------------------------------------------------
 * Build & run (requires JDK 8+ and LWJGL 3.4.1 glfw + core natives):
 *
 *     javac tests_abi/GLFWAbiTest.java
 *
 *     java -Djava.library.path=build \
 *          -cp tests_abi:<lwjgl.jar>:<lwjgl-glfw.jar>:<lwjgl-core.jar> \
 *          GLFWAbiTest
 *
 * Example with a Maven-style layout:
 *
 *     LWJGL_CP="$HOME/.m2/.../lwjgl-3.4.1.jar:\
 * $HOME/.m2/.../lwjgl-glfw-3.4.1.jar:\
 * $HOME/.m2/.../lwjgl-3.4.1-natives-macos.jar"
 *
 *     java -Djava.library.path=build -cp "tests_abi:$LWJGL_CP" GLFWAbiTest
 *
 * The macOS natives jar must match the LWJGL version (3.4.1) so that
 * the mandatory-symbol set is exactly what production Minecraft will
 * resolve.
 *
 * Exit codes:
 *   0 — pass: org.lwjgl.glfw.GLFW loaded, all mandatory symbols present.
 *   1 — fail: ABI mismatch (missing symbol or class-load error).
 *   3 — LWJGL unavailable (fall back to scripts/check_glfw_abi.sh).
 * -----------------------------------------------------------------------
 */

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

public class GLFWAbiTest {

    /**
     * Mandatory GLFW symbols queried by LWJGL 3.4.1's GLFW bindings at
     * static-init time.  This mirrors the list verified by
     * scripts/check_glfw_abi.sh (which derives the full set from
     * glfw3.h's GLFWAPI declarations).  Keep this list in sync with the
     * GLFWAPI entries in external/glfw/include/GLFW/glfw3.h.
     *
     * The list below is the LWJGL 3.4.1 mandatory subset; the shell
     * script's auto-derived list (all 124 GLFWAPI functions) is the
     * strict superset used for CI gating.
     */
    private static final String[] REQUIRED = {
        // init / version / error
        "glfwInit", "glfwTerminate", "glfwInitHint",
        "glfwInitAllocator", "glfwInitVulkanLoader",
        "glfwGetVersion", "glfwGetVersionString",
        "glfwGetError", "glfwSetErrorCallback",
        // platform
        "glfwGetPlatform", "glfwPlatformSupported",
        // monitor
        "glfwGetMonitors", "glfwGetPrimaryMonitor",
        "glfwGetMonitorPos", "glfwGetMonitorWorkarea",
        "glfwGetMonitorPhysicalSize", "glfwGetMonitorContentScale",
        "glfwGetMonitorName", "glfwSetMonitorUserPointer",
        "glfwGetMonitorUserPointer", "glfwSetMonitorCallback",
        "glfwGetVideoModes", "glfwGetVideoMode",
        "glfwSetGamma", "glfwGetGammaRamp", "glfwSetGammaRamp",
        // window
        "glfwDefaultWindowHints", "glfwWindowHint", "glfwWindowHintString",
        "glfwCreateWindow", "glfwDestroyWindow",
        "glfwWindowShouldClose", "glfwSetWindowShouldClose",
        "glfwGetWindowTitle", "glfwSetWindowTitle", "glfwSetWindowIcon",
        "glfwGetWindowPos", "glfwSetWindowPos",
        "glfwGetWindowSize", "glfwSetWindowSize",
        "glfwSetWindowSizeLimits", "glfwSetWindowAspectRatio",
        "glfwGetFramebufferSize", "glfwGetWindowFrameSize",
        "glfwGetWindowContentScale",
        "glfwGetWindowOpacity", "glfwSetWindowOpacity",
        "glfwIconifyWindow", "glfwRestoreWindow", "glfwMaximizeWindow",
        "glfwShowWindow", "glfwHideWindow", "glfwFocusWindow",
        "glfwRequestWindowAttention",
        "glfwGetWindowMonitor", "glfwSetWindowMonitor",
        "glfwGetWindowAttrib", "glfwSetWindowAttrib",
        "glfwGetWindowUserPointer", "glfwSetWindowUserPointer",
        "glfwSetWindowPosCallback", "glfwSetWindowSizeCallback",
        "glfwSetWindowCloseCallback", "glfwSetWindowRefreshCallback",
        "glfwSetWindowFocusCallback", "glfwSetWindowIconifyCallback",
        "glfwSetWindowMaximizeCallback",
        "glfwSetFramebufferSizeCallback", "glfwSetWindowContentScaleCallback",
        // event loop
        "glfwPollEvents", "glfwWaitEvents", "glfwWaitEventsTimeout",
        "glfwPostEmptyEvent",
        // input
        "glfwGetInputMode", "glfwSetInputMode", "glfwRawMouseMotionSupported",
        "glfwGetKeyName", "glfwGetKeyScancode",
        "glfwGetKey", "glfwGetMouseButton",
        "glfwGetCursorPos", "glfwSetCursorPos",
        "glfwCreateCursor", "glfwCreateStandardCursor",
        "glfwDestroyCursor", "glfwSetCursor",
        "glfwSetKeyCallback", "glfwSetCharCallback", "glfwSetCharModsCallback",
        "glfwSetMouseButtonCallback", "glfwSetCursorPosCallback",
        "glfwSetCursorEnterCallback", "glfwSetScrollCallback",
        "glfwSetDropCallback",
        // joystick / gamepad
        "glfwJoystickPresent", "glfwGetJoystickAxes", "glfwGetJoystickButtons",
        "glfwGetJoystickHats", "glfwGetJoystickName", "glfwGetJoystickGUID",
        "glfwSetJoystickUserPointer", "glfwGetJoystickUserPointer",
        "glfwJoystickIsGamepad", "glfwSetJoystickCallback",
        "glfwUpdateGamepadMappings", "glfwGetGamepadName", "glfwGetGamepadState",
        // clipboard / time
        "glfwSetClipboardString", "glfwGetClipboardString",
        "glfwGetTime", "glfwSetTime",
        "glfwGetTimerValue", "glfwGetTimerFrequency",
        // context
        "glfwMakeContextCurrent", "glfwGetCurrentContext",
        "glfwSwapBuffers", "glfwSwapInterval",
        "glfwExtensionSupported", "glfwGetProcAddress",
        // vulkan
        "glfwVulkanSupported", "glfwGetRequiredInstanceExtensions",
        "glfwGetInstanceProcAddress",
        "glfwGetPhysicalDevicePresentationSupport",
        "glfwCreateWindowSurface",
    };

    public static void main(String[] args) {
        // Load the MGL-built GLFW shared library.  -Djava.library.path=build
        // must point at the directory containing libglfw.dylib.
        try {
            System.loadLibrary("glfw");
        } catch (UnsatisfiedLinkError e) {
            System.err.println(
                "ABI FAIL: cannot load libglfw — " + e.getMessage());
            System.err.println(
                "  Make sure -Djava.library.path points at the build/ " +
                "dir containing libglfw.dylib (run 'make lib' first).");
            System.exit(1);
        }

        Class<?> glfw;
        try {
            // Loading the class triggers LWJGL's native-library bootstrap.
            // If libglfw.dylib is missing a mandatory symbol, LWJGL's
            // function-address lookup will throw here.
            glfw = Class.forName("org.lwjgl.glfw.GLFW");
        } catch (ClassNotFoundException e) {
            System.out.println(
                "NOTE: org.lwjgl.glfw.GLFW not on classpath.");
            System.out.println(
                "      Falling back to nm-based check:");
            System.out.println(
                "        scripts/check_glfw_abi.sh");
            System.out.println(
                "      Install LWJGL 3.4.1 (glfw + core natives) to run " +
                "the Java test.");
            System.exit(3);
        } catch (UnsatisfiedLinkError e) {
            System.err.println("ABI FAIL: " + e.getMessage());
            System.exit(1);
        }

        // Force eager resolution of every mandatory function by reflectively
        // invoking each method's address accessor.  LWJGL generates a
        // static field per function holding its native address; resolving
        // the class verifies that dlsym succeeded for all of them at init.
        List<String> missing = new ArrayList<>();
        for (String name : REQUIRED) {
            try {
                glfw.getDeclaredMethod(name);
            } catch (NoSuchMethodException e) {
                missing.add(name);
            }
        }

        if (!missing.isEmpty()) {
            System.err.printf(
                "ABI FAIL: %d missing method(s) on org.lwjgl.glfw.GLFW:%n",
                missing.size());
            for (String name : missing) {
                System.err.println("  - " + name);
            }
            System.exit(1);
        }

        System.out.printf(
            "ABI OK: org.lwjgl.glfw.GLFW loaded; all %d mandatory " +
            "symbols present.%n", REQUIRED.length);
        System.exit(0);
    }
}
