/*
 * Copyright (c) 2021, Gluon
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL GLUON BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "jni.h"
#include "bridge_webview.h"

extern void substrate_createWebView(jlong handle);
extern void substrate_loadUrl(jlong handle, char *c);
extern void substrate_loadContent(jlong handle, char *c);
extern void substrate_setWebViewX(jlong handle, double x);
extern void substrate_setWebViewY(jlong handle, double y);
extern void substrate_setWebViewWidth(jlong handle, double w);
extern void substrate_setWebViewHeight(jlong handle, double h);
extern void substrate_setWebViewVisible(jlong handle, jboolean visible);
extern void substrate_reloadWebView(jlong handle);
extern void substrate_removeWebView(jlong handle);
extern char* substrate_executeScript(jlong handle, char *c);

/*
 * Every javafx.scene.web.WebView instance registers itself here via _initWebView,
 * which hands a unique handle back to the JavaFX side (nativeHandle[0]). The JavaFX
 * WebView passes that handle into every subsequent native call, and the Dalvik side
 * passes it back with every callback, so concurrent/overlapping WebView lifecycles
 * (help window closed and reopened, JsonView + help coexisting) can no longer
 * cross-talk. Slots hold a Graal-side global ref to the JavaFX WebView object,
 * used to deliver the notifyLoad* callbacks to the right instance.
 */
#define MAX_WEBVIEWS 8

typedef struct {
    jlong   handle;      /* 0 = free slot */
    jobject fxWebView;   /* Graal-side global ref to javafx.scene.web.WebView */
} WebViewSlot;

static WebViewSlot slots[MAX_WEBVIEWS];
static jlong nextHandle = 1;
static pthread_mutex_t slotsMutex = PTHREAD_MUTEX_INITIALIZER;

static jmethodID jmidLoadStarted = NULL;
static jmethodID jmidLoadFinished = NULL;
static jmethodID jmidLoadFailed = NULL;
static jmethodID jmidJavaCall = NULL;

static JavaVM *jvm;

static jclass graalAndroidWebViewEngineClass = NULL;
static int engineClassLookupFailed = 0;
static jmethodID jmidGetResourceBytes = NULL;
static int resourceEngineLookupFailed = 0;
static jmethodID jmidDispatchBridgeCall = NULL;
static int bridgeDispatchLookupFailed = 0;

JavaVM* getWebViewGraalVM() {
    return jvm;
}

static int checkAndClearGraalException(JNIEnv *env, const char *where) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        fprintf(stderr, "WebView: Graal JNI exception in %s (cleared)\n", where);
        return 1;
    }
    return 0;
}

static jlong registerWebView(JNIEnv *env, jobject fxWebView) {
    pthread_mutex_lock(&slotsMutex);
    jlong handle = nextHandle++;
    for (int i = 0; i < MAX_WEBVIEWS; i++) {
        if (slots[i].handle == 0) {
            slots[i].handle = handle;
            slots[i].fxWebView = (*env)->NewGlobalRef(env, fxWebView);
            pthread_mutex_unlock(&slotsMutex);
            fprintf(stderr, "WebView: registered handle %lld in slot %d\n", (long long) handle, i);
            return handle;
        }
    }
    pthread_mutex_unlock(&slotsMutex);
    fprintf(stderr, "WebView: slot table full, more than %d live WebViews\n", MAX_WEBVIEWS);
    return 0;
}

static jobject fxWebViewForHandle(jlong handle) {
    pthread_mutex_lock(&slotsMutex);
    for (int i = 0; i < MAX_WEBVIEWS; i++) {
        if (slots[i].handle == handle) {
            jobject result = slots[i].fxWebView;
            pthread_mutex_unlock(&slotsMutex);
            return result;
        }
    }
    pthread_mutex_unlock(&slotsMutex);
    return NULL;
}

static void unregisterWebView(JNIEnv *env, jlong handle) {
    pthread_mutex_lock(&slotsMutex);
    for (int i = 0; i < MAX_WEBVIEWS; i++) {
        if (slots[i].handle == handle) {
            (*env)->DeleteGlobalRef(env, slots[i].fxWebView);
            slots[i].fxWebView = NULL;
            slots[i].handle = 0;
            break;
        }
    }
    pthread_mutex_unlock(&slotsMutex);
}

static void initializeWebViewMethodIds(JNIEnv *env, jobject fxWebView) {
    if (jmidLoadStarted != NULL) {
        return;
    }
    jclass webViewClass = (*env)->GetObjectClass(env, fxWebView);
    jmidLoadStarted = (*env)->GetMethodID(env, webViewClass, "notifyLoadStarted", "()V");
    jmidLoadFinished = (*env)->GetMethodID(env, webViewClass, "notifyLoadFinished", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmidLoadFailed = (*env)->GetMethodID(env, webViewClass, "notifyLoadFailed", "()V");
    jmidJavaCall = (*env)->GetMethodID(env, webViewClass, "notifyJavaCall", "(Ljava/lang/String;)V");
    checkAndClearGraalException(env, "initializeWebViewMethodIds");
}

jint JNI_OnLoad_webview(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_6)) {
        return JNI_ERR; /* JNI version not supported */
    }
    jvm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
    Java_javafx_scene_web_WebView__1initWebView(JNIEnv *env, jobject obj, jlongArray nativeHandle) {
    initializeWebViewMethodIds(env, obj);
    jlong handle = registerWebView(env, obj);
    /* The WebView constructor reads nativeHandle[0] immediately after this
     * returns; it must be written synchronously. */
    (*env)->SetLongArrayRegion(env, nativeHandle, 0, 1, &handle);
    fprintf(stderr, "WebView, initWebView handle %lld\n", (long long) handle);
    substrate_createWebView(handle);
}

JNIEXPORT void JNICALL
    Java_javafx_scene_web_WebEngine__1loadUrl(JNIEnv *env, jobject cl, jlong handle, jstring str) {
    char *curl = (char *)(*env)->GetStringUTFChars(env, str, JNI_FALSE);
    substrate_loadUrl(handle, curl);
    (*env)->ReleaseStringUTFChars(env, str, curl);
}

JNIEXPORT jstring JNICALL
    Java_javafx_scene_web_WebEngine__1executeScript(JNIEnv *env, jobject cl, jlong handle, jstring script) {
    char *cscript = (char *)(*env)->GetStringUTFChars(env, script, JNI_FALSE);
    char *result = substrate_executeScript(handle, cscript);
    (*env)->ReleaseStringUTFChars(env, script, cscript);
    if (result == NULL) {
        return NULL;
    }
    jstring jresult = (*env)->NewStringUTF(env, result);
    free(result);
    return jresult;
}

JNIEXPORT void JNICALL
    Java_javafx_scene_web_WebEngine__1loadContent(JNIEnv *env, jobject cl, jlong handle, jstring content) {
    char *ccontent = (char *)(*env)->GetStringUTFChars(env, content, JNI_FALSE);
    substrate_loadContent(handle, ccontent);
    (*env)->ReleaseStringUTFChars(env, content, ccontent);
}

JNIEXPORT void JNICALL
    Java_javafx_scene_web_WebEngine__1reload(JNIEnv *env, jobject cl, jlong handle) {
    substrate_reloadWebView(handle);
}

JNIEXPORT void JNICALL
    Java_javafx_scene_web_WebView__1setWidth(JNIEnv *env, jobject cl, jlong handle, jdouble w) {
    substrate_setWebViewWidth(handle, w);
}

JNIEXPORT void JNICALL
    Java_javafx_scene_web_WebView__1setHeight(JNIEnv *env, jobject cl, jlong handle, jdouble h) {
    substrate_setWebViewHeight(handle, h);
}

JNIEXPORT void JNICALL
    Java_javafx_scene_web_WebView__1setVisible(JNIEnv *env, jobject cl, jlong handle, jboolean v) {
    substrate_setWebViewVisible(handle, v);
}

JNIEXPORT void JNICALL
    Java_javafx_scene_web_WebView__1removeWebView(JNIEnv *env, jobject cl, jlong handle) {
    fprintf(stderr, "WebView, removeWebView handle %lld\n", (long long) handle);
    substrate_removeWebView(handle);
    unregisterWebView(env, handle);
}

JNIEXPORT void JNICALL
    Java_javafx_scene_web_WebView__1setTransform(JNIEnv *env, jobject cl, jlong handle,
        jdouble mxx, jdouble mxy, jdouble mxz, jdouble mxt,
        jdouble myx, jdouble myy, jdouble myz, jdouble myt,
        jdouble mzx, jdouble mzy, jdouble mzz, jdouble mzt) {
    /* Only the translation components of the node-to-scene transform are
     * consumed; the WebView must not sit inside scaled/rotated ancestors. */
    substrate_setWebViewX(handle, mxt);
    substrate_setWebViewY(handle, myt);
}

void androidJfx_startURL(jlong handle, const char *url) {
    ATTACH_GRAAL();
    jobject fxWebView = fxWebViewForHandle(handle);
    if (fxWebView != NULL) {
        (*graalEnv)->CallVoidMethod(graalEnv, fxWebView, jmidLoadStarted);
        checkAndClearGraalException(graalEnv, "androidJfx_startURL");
    }
    DETACH_GRAAL();
}

void androidJfx_finishURL(jlong handle, const char *url, const char *html) {
    ATTACH_GRAAL();
    jobject fxWebView = fxWebViewForHandle(handle);
    if (fxWebView != NULL) {
        jstring jurl = (*graalEnv)->NewStringUTF(graalEnv, url);
        jstring jhtml = (*graalEnv)->NewStringUTF(graalEnv, html);
        (*graalEnv)->CallVoidMethod(graalEnv, fxWebView, jmidLoadFinished, jurl, jhtml);
        checkAndClearGraalException(graalEnv, "androidJfx_finishURL");
        (*graalEnv)->DeleteLocalRef(graalEnv, jurl);
        (*graalEnv)->DeleteLocalRef(graalEnv, jhtml);
    }
    DETACH_GRAAL();
}

void androidJfx_failedURL(jlong handle, const char *url) {
    ATTACH_GRAAL();
    jobject fxWebView = fxWebViewForHandle(handle);
    if (fxWebView != NULL) {
        (*graalEnv)->CallVoidMethod(graalEnv, fxWebView, jmidLoadFailed);
        checkAndClearGraalException(graalEnv, "androidJfx_failedURL");
    }
    DETACH_GRAAL();
}

void androidJfx_javaCallURL(jlong handle, const char *url) {
    ATTACH_GRAAL();
    jobject fxWebView = fxWebViewForHandle(handle);
    if (fxWebView != NULL) {
        jstring jurl = (*graalEnv)->NewStringUTF(graalEnv, url);
        (*graalEnv)->CallVoidMethod(graalEnv, fxWebView, jmidJavaCall, jurl);
        checkAndClearGraalException(graalEnv, "androidJfx_javaCallURL");
        (*graalEnv)->DeleteLocalRef(graalEnv, jurl);
    }
    DETACH_GRAAL();
}

/* Lazily resolves (and caches, as a global ref) the application's bridge endpoint class.
 * Class not present or not registered for JNI in the native image (needs a jni-config
 * entry) => fail once, loudly, and don't retry. */
static jclass ensureAndroidWebViewEngineClass(JNIEnv *graalEnv) {
    if (graalAndroidWebViewEngineClass == NULL && !engineClassLookupFailed) {
        jclass cls = (*graalEnv)->FindClass(graalEnv, "za/co/embrace/desktop/utilities/provided/AndroidWebViewEngine");
        if (checkAndClearGraalException(graalEnv, "ensureEngineClass:FindClass") || cls == NULL) {
            fprintf(stderr, "WebView: AndroidWebViewEngine not found; resource: URLs and the embraceNative bridge will not work\n");
            engineClassLookupFailed = 1;
        } else {
            graalAndroidWebViewEngineClass = (jclass)(*graalEnv)->NewGlobalRef(graalEnv, cls);
        }
    }
    return graalAndroidWebViewEngineClass;
}

char* substrate_loadResourceBytes(const char *path, int *outLength) {
    ATTACH_GRAAL();
    *outLength = 0;
    if (jmidGetResourceBytes == NULL && !resourceEngineLookupFailed) {
        jclass cls = ensureAndroidWebViewEngineClass(graalEnv);
        if (cls == NULL) {
            resourceEngineLookupFailed = 1;
        } else {
            jmidGetResourceBytes = (*graalEnv)->GetStaticMethodID(graalEnv, cls,
                    "getResourceBytes", "(Ljava/lang/String;)[B");
            if (checkAndClearGraalException(graalEnv, "loadResourceBytes:GetStaticMethodID") || jmidGetResourceBytes == NULL) {
                resourceEngineLookupFailed = 1;
            }
        }
    }
    if (resourceEngineLookupFailed) {
        DETACH_GRAAL();
        return NULL;
    }
    jstring jpath = (*graalEnv)->NewStringUTF(graalEnv, path);
    jbyteArray jdata = (jbyteArray)(*graalEnv)->CallStaticObjectMethod(graalEnv, graalAndroidWebViewEngineClass, jmidGetResourceBytes, jpath);
    checkAndClearGraalException(graalEnv, "loadResourceBytes:getResourceBytes");
    (*graalEnv)->DeleteLocalRef(graalEnv, jpath);
    char *result = NULL;
    if (jdata != NULL) {
        jsize length = (*graalEnv)->GetArrayLength(graalEnv, jdata);
        *outLength = (int)length;
        result = (char*)malloc(length);
        if (result != NULL) {
            (*graalEnv)->GetByteArrayRegion(graalEnv, jdata, 0, length, (jbyte*)result);
        } else {
            *outLength = 0;
        }
    }
    DETACH_GRAAL();
    return result;
}

JNIEXPORT jbyteArray JNICALL
    Java_com_gluonhq_helloandroid_NativeWebView_loadResourceBytes(JNIEnv *env, jobject obj, jstring jpath) {
    const char *path = (*env)->GetStringUTFChars(env, jpath, JNI_FALSE);
    int length = 0;
    char *data = substrate_loadResourceBytes(path, &length);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (data == NULL) {
        return NULL;
    }
    jbyteArray result = (*env)->NewByteArray(env, length);
    if (result != NULL) {
        (*env)->SetByteArrayRegion(env, result, 0, length, (jbyte*)data);
    }
    free(data);
    return result;
}

/*
 * window.embraceNative bridge (call/post from page JS, see NativeWebView.java).
 * Forwards to the application's dispatch hook on the Graal side:
 *   AndroidWebViewEngine.dispatchBridgeCall(long handle, String method, String argsJson)
 * Returns a malloc'd copy of the handler's result (caller frees), or NULL if the hook is
 * absent (older application jar - degrades gracefully, resource serving is unaffected) or
 * the handler returned null.
 */
static char* substrate_dispatchBridgeCall(jlong handle, const char *method, const char *argsJson) {
    ATTACH_GRAAL();
    if (jmidDispatchBridgeCall == NULL && !bridgeDispatchLookupFailed) {
        jclass cls = ensureAndroidWebViewEngineClass(graalEnv);
        if (cls == NULL) {
            bridgeDispatchLookupFailed = 1;
        } else {
            jmidDispatchBridgeCall = (*graalEnv)->GetStaticMethodID(graalEnv, cls,
                    "dispatchBridgeCall", "(JLjava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
            if (checkAndClearGraalException(graalEnv, "dispatchBridgeCall:GetStaticMethodID") || jmidDispatchBridgeCall == NULL) {
                fprintf(stderr, "WebView: AndroidWebViewEngine.dispatchBridgeCall not found; embraceNative bridge disabled\n");
                bridgeDispatchLookupFailed = 1;
            }
        }
    }
    if (bridgeDispatchLookupFailed) {
        DETACH_GRAAL();
        return NULL;
    }
    jstring jmethod = (*graalEnv)->NewStringUTF(graalEnv, method);
    jstring jargs = argsJson != NULL ? (*graalEnv)->NewStringUTF(graalEnv, argsJson) : NULL;
    jstring jresult = (jstring)(*graalEnv)->CallStaticObjectMethod(graalEnv, graalAndroidWebViewEngineClass,
            jmidDispatchBridgeCall, handle, jmethod, jargs);
    checkAndClearGraalException(graalEnv, "dispatchBridgeCall:call");
    char *result = NULL;
    if (jresult != NULL) {
        const char *cresult = (*graalEnv)->GetStringUTFChars(graalEnv, jresult, JNI_FALSE);
        result = strdup(cresult);
        (*graalEnv)->ReleaseStringUTFChars(graalEnv, jresult, cresult);
        (*graalEnv)->DeleteLocalRef(graalEnv, jresult);
    }
    (*graalEnv)->DeleteLocalRef(graalEnv, jmethod);
    if (jargs != NULL) {
        (*graalEnv)->DeleteLocalRef(graalEnv, jargs);
    }
    DETACH_GRAAL();
    return result;
}

JNIEXPORT jstring JNICALL
    Java_com_gluonhq_helloandroid_NativeWebView_nativeBridgeCall(JNIEnv *env, jobject obj,
        jlong handle, jstring jmethod, jstring jargs) {
    const char *method = (*env)->GetStringUTFChars(env, jmethod, JNI_FALSE);
    const char *args = jargs != NULL ? (*env)->GetStringUTFChars(env, jargs, JNI_FALSE) : NULL;
    char *result = substrate_dispatchBridgeCall(handle, method, args);
    (*env)->ReleaseStringUTFChars(env, jmethod, method);
    if (args != NULL) {
        (*env)->ReleaseStringUTFChars(env, jargs, args);
    }
    if (result == NULL) {
        return NULL;
    }
    jstring jresult = (*env)->NewStringUTF(env, result);
    free(result);
    return jresult;
}
