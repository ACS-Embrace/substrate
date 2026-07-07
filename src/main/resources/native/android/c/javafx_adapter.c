/*
 * Copyright (c) 2020, 2022, Gluon
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
#include <stdatomic.h>
#include <time.h>
#include <EGL/egl.h>
#include "grandroid.h"

// 1 while a valid EGL surface exists, 0 after nativeSetSurface(null).
// Prevents the Adreno EGL spin-loop: on Snapdragon devices the JavaFX render
// thread keeps calling eglSwapBuffers after surface destruction, causing the
// Adreno GPU driver to SIGKILL the process (~497-1026 EGL_BAD_SURFACE errors/s).
static atomic_int egl_surface_valid = ATOMIC_VAR_INIT(0);

// Resolved by the --wrap linker mechanism to the real libEGL eglSwapBuffers.
extern EGLBoolean __real_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);

// Intercepts all eglSwapBuffers calls in the linked binary (including the
// pre-built JavaFX static SDK) via -Wl,--wrap=eglSwapBuffers.
EGLBoolean __wrap_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!atomic_load_explicit(&egl_surface_valid, memory_order_acquire)) {
        struct timespec ts = {0, 16000000L};
        nanosleep(&ts, NULL);
        return EGL_FALSE;
    }
    return __real_eglSwapBuffers(dpy, surface);
}

#ifdef JAVAFX_WEB
/*
 * All calls into the Dalvik NativeWebView go through static router methods keyed
 * by the handle that javafx.scene.web.WebView threads through every native call
 * (see bridge_webview.c). The instance map lives on the Dalvik side.
 */
jclass nativeWebViewClass;
jmethodID nativeWebView_create;
jmethodID nativeWebView_loadUrl;
jmethodID nativeWebView_loadContent;
jmethodID nativeWebView_x;
jmethodID nativeWebView_y;
jmethodID nativeWebView_width;
jmethodID nativeWebView_height;
jmethodID nativeWebView_visible;
jmethodID nativeWebView_executeScript;
jmethodID nativeWebView_reload;
jmethodID nativeWebView_remove;
int reg = -1;

static int checkAndClearDalvikException(JNIEnv *env, const char *where) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        LOGE(stderr, "WebView: Dalvik JNI exception in %s (cleared)\n", where);
        return 1;
    }
    return 0;
}

void registerJavaFXMethodHandles(JNIEnv *aenv)
{
    if (reg < 0) {
        nativeWebViewClass = (*aenv)->NewGlobalRef(aenv, (*aenv)->FindClass(aenv, "com/gluonhq/helloandroid/NativeWebView"));
        nativeWebView_create = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "create", "(J)V");
        nativeWebView_loadUrl = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "loadUrl", "(JLjava/lang/String;)V");
        nativeWebView_loadContent = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "loadContent", "(JLjava/lang/String;)V");
        nativeWebView_x = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "setX", "(JD)V");
        nativeWebView_y = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "setY", "(JD)V");
        nativeWebView_width = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "setWidth", "(JD)V");
        nativeWebView_height = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "setHeight", "(JD)V");
        nativeWebView_visible = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "setVisible", "(JZ)V");
        nativeWebView_executeScript = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "executeScript", "(JLjava/lang/String;)Ljava/lang/String;");
        nativeWebView_reload = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "reload", "(J)V");
        nativeWebView_remove = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "remove", "(J)V");
        checkAndClearDalvikException(aenv, "registerJavaFXMethodHandles");
        reg = 1;
    }
}
#else
void registerJavaFXMethodHandles(JNIEnv *aenv) {}
#endif

JNIEXPORT void JNICALL Java_com_gluonhq_helloandroid_MainActivity_nativeSetSurface(JNIEnv *env, jobject activity, jobject surface)
{
    LOGE(stderr, "nativeSetSurface called, env at %p and size %ld, surface at %p\n", env, sizeof(JNIEnv), surface);
    if (surface != NULL) {
        window = ANativeWindow_fromSurface(env, surface);
        androidJfx_setNativeWindow(window);
        atomic_store_explicit(&egl_surface_valid, 1, memory_order_release);
        LOGE(stderr, "native setSurface Ready, native window at %p\n", window);
    } else {
        atomic_store_explicit(&egl_surface_valid, 0, memory_order_release);
        androidJfx_setNativeWindow(NULL);
        LOGE(stderr, "native setSurface was null");
    }
}

JNIEXPORT jlong JNICALL Java_com_gluonhq_helloandroid_MainActivity_surfaceReady(JNIEnv *env, jobject activity, jobject surface, jfloat mydensity)
{
    LOGE(stderr, "SurfaceReady, surface at %p\n", surface);
    window = ANativeWindow_fromSurface(env, surface);
    androidJfx_setNativeWindow(window);
    androidJfx_setDensity(mydensity);
    LOGE(stderr, "SurfaceReady, native window at %p\n", window);
    density = mydensity;
    return (jlong)window;
}

JNIEXPORT void JNICALL Java_com_gluonhq_helloandroid_MainActivity_nativeSurfaceRedrawNeeded(JNIEnv *env, jobject activity)
{
    LOGE(stderr, "launcher, nativeSurfaceRedrawNeeded called. Invoke method on glass_monocle\n");
    androidJfx_requestGlassToRedraw();
}

JNIEXPORT jint JNICALL
JNI_OnLoad_javafx_font(JavaVM *vm, void *reserved)
{
    LOGE(stderr, "In dummy JNI_OnLoad_javafx_font\n");
#ifdef JNI_VERSION_1_8
    //min. returned JNI_VERSION required by JDK8 for builtin libraries
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK)
    {
        return JNI_VERSION_1_4;
    }
    return JNI_VERSION_1_8;
#else
    return JNI_VERSION_1_4;
#endif
}

void showSoftwareKeyboard()
{
    ATTACH_DALVIK();
    LOGE(stderr, "now I have to show keyboard, invoke method %p on env %p\n", activity_showIME, dalvikEnv);
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, activityClass, activity_showIME);
    LOGE(stderr, "I did show keyboard\n");
    DETACH_DALVIK();
}

JNIEXPORT void JNICALL
Java_javafx_scene_control_skin_TextAreaSkinAndroid_showSoftwareKeyboard(JNIEnv *env, jobject textareaskin)
{
    showSoftwareKeyboard();
}

JNIEXPORT void JNICALL
Java_javafx_scene_control_skin_TextFieldSkinAndroid_showSoftwareKeyboard(JNIEnv *env, jobject textfieldskin)
{
    showSoftwareKeyboard();
}

void hideSoftwareKeyboard()
{
    ATTACH_DALVIK();
    LOGE(stderr, "now I have to hide keyboard, invoke method %p on env %p\n", activity_hideIME, dalvikEnv);
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, activityClass, activity_hideIME);
    LOGE(stderr, "I did hide keyboard\n");
    DETACH_DALVIK();
}

JNIEXPORT void JNICALL
Java_javafx_scene_control_skin_TextFieldSkinAndroid_hideSoftwareKeyboard(JNIEnv *env, jobject textfieldskin)
{
    hideSoftwareKeyboard();
}

JNIEXPORT void JNICALL
Java_javafx_scene_control_skin_TextAreaSkinAndroid_hideSoftwareKeyboard(JNIEnv *env, jobject textareaskin)
{
    hideSoftwareKeyboard();
}

#ifdef JAVAFX_WEB
void substrate_createWebView(jlong handle) {
    LOGE(stderr, "Substrate creating WebView, handle %lld\n", (long long) handle);
    ATTACH_DALVIK();
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_create, handle);
    checkAndClearDalvikException(dalvikEnv, "createWebView");
    DETACH_DALVIK();
}

void substrate_loadUrl(jlong handle, char* curl) {
    ATTACH_DALVIK();
    LOGE(stderr, "load url: %s\n", curl);
    jstring jurl = (*dalvikEnv)->NewStringUTF(dalvikEnv, curl);
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_loadUrl, handle, jurl);
    checkAndClearDalvikException(dalvikEnv, "loadUrl");
    (*dalvikEnv)->DeleteLocalRef(dalvikEnv, jurl);
    DETACH_DALVIK();
}

void substrate_loadContent(jlong handle, char* content) {
    ATTACH_DALVIK();
    jstring jcontent = (*dalvikEnv)->NewStringUTF(dalvikEnv, content);
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_loadContent, handle, jcontent);
    checkAndClearDalvikException(dalvikEnv, "loadContent");
    (*dalvikEnv)->DeleteLocalRef(dalvikEnv, jcontent);
    DETACH_DALVIK();
}

/* Coordinates and sizes from JavaFX are logical pixels; the Android view
 * hierarchy works in physical pixels, hence the density multiplication
 * (density is set in surfaceReady). */
void substrate_setWebViewX(jlong handle, double x) {
    ATTACH_DALVIK();
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_x, handle, x * density);
    checkAndClearDalvikException(dalvikEnv, "setWebViewX");
    DETACH_DALVIK();
}

void substrate_setWebViewY(jlong handle, double y) {
    ATTACH_DALVIK();
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_y, handle, y * density);
    checkAndClearDalvikException(dalvikEnv, "setWebViewY");
    DETACH_DALVIK();
}

void substrate_setWebViewWidth(jlong handle, double width) {
    ATTACH_DALVIK();
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_width, handle, width * density);
    checkAndClearDalvikException(dalvikEnv, "setWebViewWidth");
    DETACH_DALVIK();
}

void substrate_setWebViewHeight(jlong handle, double height) {
    ATTACH_DALVIK();
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_height, handle, height * density);
    checkAndClearDalvikException(dalvikEnv, "setWebViewHeight");
    DETACH_DALVIK();
}

void substrate_setWebViewVisible(jlong handle, jboolean visible) {
    ATTACH_DALVIK();
    LOGE(stderr, "webView visible %d\n", (visible ? 1 : 0));
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_visible, handle, visible);
    checkAndClearDalvikException(dalvikEnv, "setWebViewVisible");
    DETACH_DALVIK();
}

/* Returns a malloc'ed copy the caller must free, or NULL. The string contents
 * must be copied before DETACH_DALVIK: the JNI string memory is only valid
 * while attached. */
char* substrate_executeScript(jlong handle, char* script) {
    ATTACH_DALVIK();
    jstring jscript = (*dalvikEnv)->NewStringUTF(dalvikEnv, script);
    jstring result = (jstring)(*dalvikEnv)->CallStaticObjectMethod(dalvikEnv, nativeWebViewClass,
            nativeWebView_executeScript, handle, jscript);
    (*dalvikEnv)->DeleteLocalRef(dalvikEnv, jscript);
    char *copy = NULL;
    if (!checkAndClearDalvikException(dalvikEnv, "executeScript") && result != NULL) {
        const char *resultChars = (*dalvikEnv)->GetStringUTFChars(dalvikEnv, result, NULL);
        if (resultChars != NULL) {
            copy = strdup(resultChars);
            (*dalvikEnv)->ReleaseStringUTFChars(dalvikEnv, result, resultChars);
        }
    }
    DETACH_DALVIK();
    return copy;
}

void substrate_reloadWebView(jlong handle) {
    ATTACH_DALVIK();
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_reload, handle);
    checkAndClearDalvikException(dalvikEnv, "reloadWebView");
    DETACH_DALVIK();
}

void substrate_removeWebView(jlong handle) {
    ATTACH_DALVIK();
    LOGE(stderr, "remove webView, handle %lld\n", (long long) handle);
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_remove, handle);
    checkAndClearDalvikException(dalvikEnv, "removeWebView");
    DETACH_DALVIK();
}

// Callbacks

JNIEXPORT void JNICALL Java_com_gluonhq_helloandroid_NativeWebView_nativeStartURL(JNIEnv *env, jobject obj, jlong handle, jstring url)
{
    const char *curl = (*env)->GetStringUTFChars(env, url, NULL);
    LOGE(stderr, "nativeStartURL called. URL: %s\n", curl);
    androidJfx_startURL(handle, curl);
    (*env)->ReleaseStringUTFChars(env, url, curl);
}

JNIEXPORT void JNICALL Java_com_gluonhq_helloandroid_NativeWebView_nativeFinishURL(JNIEnv *env, jobject obj, jlong handle, jstring url, jstring html)
{
    const char *curl = (*env)->GetStringUTFChars(env, url, NULL);
    const char *chtml = (*env)->GetStringUTFChars(env, html, NULL);
    androidJfx_finishURL(handle, curl, chtml);
    (*env)->ReleaseStringUTFChars(env, url, curl);
    (*env)->ReleaseStringUTFChars(env, html, chtml);
}

JNIEXPORT void JNICALL Java_com_gluonhq_helloandroid_NativeWebView_nativeFailedURL(JNIEnv *env, jobject obj, jlong handle, jstring url)
{
    const char *curl = (*env)->GetStringUTFChars(env, url, NULL);
    androidJfx_failedURL(handle, curl);
    (*env)->ReleaseStringUTFChars(env, url, curl);
}

JNIEXPORT void JNICALL Java_com_gluonhq_helloandroid_NativeWebView_nativeJavaCallURL(JNIEnv *env, jobject obj, jlong handle, jstring url)
{
    const char *curl = (*env)->GetStringUTFChars(env, url, NULL);
    androidJfx_javaCallURL(handle, curl);
    (*env)->ReleaseStringUTFChars(env, url, curl);
}

#endif