/*
 * mgl_config.m
 * MGL - Java system property to environment bridge.
 *
 * Allows MGL configuration via -D JVM flags (e.g. -DMGL_PERF_SUMMARY=1)
 * by syncing Java system properties to C environment variables at
 * createGLMContext time.  getenv() calls throughout MGL then work
 * transparently with both -D flags and real env vars.
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <jni.h>

/* Locate the running JVM via JNI_GetCreatedJavaVMs.  libmgl is loaded
 * by LWJGL via dlopen (not System.loadLibrary), so JNI_OnLoad is never
 * called.  Instead we dlsym the JVM symbol at first use. */
static JavaVM *mglGetJVM(void)
{
    static JavaVM *jvm = NULL;
    static bool tried = false;
    if (tried) return jvm;
    tried = true;

    typedef jint (*GetVMsFn)(JavaVM **, jsize, jsize *);

    /* RTLD_DEFAULT searches all globally-loaded images — libjvm.dylib
     * is loaded globally by the Java launcher. */
    GetVMsFn fn = (GetVMsFn)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    if (!fn) {
        /* Fallback: explicitly open libjvm.dylib (RTLD_NOLOAD = only if
         * already loaded, no new mapping). */
        void *handle = dlopen("libjvm.dylib", RTLD_LAZY | RTLD_NOLOAD);
        if (!handle) handle = dlopen("libjvm.dylib", RTLD_LAZY);
        if (handle) fn = (GetVMsFn)dlsym(handle, "JNI_GetCreatedJavaVMs");
    }
    if (!fn) return NULL;

    jsize count = 0;
    if (fn(&jvm, 1, &count) != JNI_OK || count == 0) {
        jvm = NULL;
    }
    return jvm;
}

/* All MGL_ config keys that can be set via -D JVM flags.  Keep in sync
 * with getenv("MGL_...") call sites.  Real env vars take priority. */
static const char *kMGLConfigKeys[] = {
    "MGL_PERF_SUMMARY",
    "MGL_PERF_SUMMARY_EVERY",
    "MGL_PERF_SUMMARY_UNSAFE_EVERY_FRAME",
    "MGL_PERF_LOCK_TIMING",
    "MGL_SIGNPOST",
    "MGL_TRACE_FBO_STATUS",
    "MGL_TRACE_TEXTURE_NAMES",
    "MGL_TRACE_LOG_PROGRAMS",
    "MGL_SYNC_STRICT",
    "MGL_VALIDATE_CURRENT_CONTEXT",
    "MGL_DISABLE_DRAW_DEFER",
    "MGL_DISABLE_MTL4_COMPILER",
    "MGL_DISABLE_FINE_TEXTURE_PARAM_FLUSH",
    "MGL_DISABLE_ZERO_UPLOAD_JAVA_STACK",
    "MGL_DISABLE_IR_REMAP",
    "MGL_DISABLE_STREAM_MERGE",
    "MGL_DISABLE_STREAM_MERGE_EXCLUSIONS",
    "MGL_PARALLEL_ENCODE",
    "MGL_ENABLE_INDIRECT_COUNT_GPU",
    "MGL_BIND_NO_FLUSH",
    "MGL_DEBUG_STRUCT_PACK",
    "MGL_DEBUG_IR_REMAP",
    "MGL_DEBUG_MSL_PACK",
    "MGL_DEBUG_UBO_REFLECT",
    "MGL_DEBUG_STREAM_MERGE",
    "MGL_DUMP_MSL",
    "MGL_DUMP_MSL_POST_PACK",
    "MGL_STRICT_TEXTURE_ERRORS",
    "MGL_ASSERT_NO_MSL_BINDING_REWRITE",
    "MGL_XFB_GPU_CAPTURE",
    "MGL_CULL_DBG",
    NULL
};

void mglSyncJavaPropertiesToEnv(void)
{
    JavaVM *jvm = mglGetJVM();
    if (!jvm) return;

    JNIEnv *env = NULL;
    jint rc = (*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        rc = (*jvm)->AttachCurrentThread(jvm, (void **)&env, NULL);
        if (rc != JNI_OK || !env) return;
    } else if (rc != JNI_OK || !env) {
        return;
    }

    jclass systemClass = (*env)->FindClass(env, "java/lang/System");
    if (!systemClass) {
        (*env)->ExceptionClear(env);
        return;
    }
    jmethodID getProperty = (*env)->GetStaticMethodID(env, systemClass,
        "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!getProperty) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, systemClass);
        return;
    }

    for (int i = 0; kMGLConfigKeys[i] != NULL; i++) {
        const char *key = kMGLConfigKeys[i];
        if (getenv(key)) continue;  /* real env var wins */

        jstring jkey = (*env)->NewStringUTF(env, key);
        if (!jkey) {
            (*env)->ExceptionClear(env);
            continue;
        }
        jstring jval = (jstring)(*env)->CallStaticObjectMethod(env,
            systemClass, getProperty, jkey);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, jkey);
            continue;
        }
        if (jval) {
            const char *val = (*env)->GetStringUTFChars(env, jval, NULL);
            if (val) {
                setenv(key, val, 1);
                (*env)->ReleaseStringUTFChars(env, jval, val);
            }
            (*env)->DeleteLocalRef(env, jval);
        }
        (*env)->DeleteLocalRef(env, jkey);
    }

    (*env)->DeleteLocalRef(env, systemClass);
}
