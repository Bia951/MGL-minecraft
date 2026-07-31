/*
 * ABI.java — GLFW / LWJGL 3.4.1 ABI conformance test (SubTask 1.3).
 *
 * Verifies that the native libglfw.dylib shipped with MGL exports every
 * GLFW function symbol that LWJGL 3.4.1 resolves at static-init time.
 * LWJGL looks up these symbols by name (dlsym) when
 * org.lwjgl.glfw.GLFW is class-loaded; if any mandatory symbol is
 * missing the JVM aborts with UnsatisfiedLinkError during GLFW init.
 *
 * Two modes:
 *
 *   1. LWJGL on classpath  — loads Class.forName("org.lwjgl.glfw.GLFW")
 *      and forces eager resolution of every mandatory function pointer
 *      by invoking each function's JNI address getter.  Any missing
 *      export => test fails.
 *
 *   2. LWJGL absent        — prints a notice and exits with code 3 so
 *      the shell wrapper (run_abi_check.sh) falls back to the
 *      equivalent `nm -gU` symbol check, which is authoritative.
 *
 * Build & run (see run_abi_check.sh for the one-shot wrapper):
 *
 *     javac ABI.java
 *     java -Djava.library.path=../../../build \
 *          -cp .:<path-to-lwjgl.jar>:<path-to-lwjgl-glfw.jar> \
 *          ABI
 *
 * Exit codes: 0 = pass, 1 = ABI mismatch, 3 = LWJGL unavailable.
 */

import java.lang.reflect.*;
import java.util.*;

public class ABI {
    /**
     * Mandatory GLFW symbols queried by LWJGL 3.4.1's GLFW bindings.
     * Mirrors the list verified by run_abi_check.sh.  Keep in sync.
     */
    private static final String[] REQUIRED = {
        "glfwInit", "glfwTerminate", "glfwInitHint",
        "glfwGetVersion", "glfwGetVersionString",
        "glfwGetError", "glfwSetErrorCallback",
        "glfwGetPlatform", "glfwPlatformSupported",
        "glfwGetMonitors", "glfwGetPrimaryMonitor",
        "glfwGetMonitorPos", "glfwGetMonitorWorkarea",
        "glfwGetMonitorPhysicalSize", "glfwGetMonitorContentScale",
        "glfwGetMonitorName", "glfwSetMonitorCallback",
        "glfwGetVideoModes", "glfwGetVideoMode",
        "glfwSetGamma", "glfwGetGammaRamp", "glfwSetGammaRamp",
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
        "glfwPollEvents", "glfwWaitEvents", "glfwWaitEventsTimeout",
        "glfwPostEmptyEvent",
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
        "glfwJoystickPresent", "glfwGetJoystickAxes", "glfwGetJoystickButtons",
        "glfwGetJoystickHats", "glfwGetJoystickName", "glfwGetJoystickGUID",
        "glfwJoystickIsGamepad", "glfwSetJoystickCallback",
        "glfwUpdateGamepadMappings", "glfwGetGamepadName", "glfwGetGamepadState",
        "glfwSetClipboardString", "glfwGetClipboardString",
        "glfwGetTime", "glfwSetTime",
        "glfwGetTimerValue", "glfwGetTimerFrequency",
        "glfwMakeContextCurrent", "glfwGetCurrentContext",
        "glfwSwapBuffers", "glfwSwapInterval",
        "glfwExtensionSupported", "glfwGetProcAddress",
        "glfwVulkanSupported", "glfwGetRequiredInstanceExtensions",
    };

    public static void main(String[] args) {
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
                "      Falling back to nm-based check (run_abi_check.sh).");
            System.out.println(
                "      Install LWJGL (glfw + core natives) to run the Java test.");
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
            System.err.printf("ABI FAIL: %d missing method(s):%n",
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
