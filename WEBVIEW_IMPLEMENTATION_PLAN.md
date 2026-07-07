# Native WebView Implementation Plan (Android-first, iOS-ready)

Goal: reliably display interactive help content (and later the AI panel) in a native
Android WebView occupying a *section* of the JavaFX scene, loaded from resources
bundled in the EmbraceDesktop jar, with JS execution support.

Scope decisions (agreed 2026-07-07):
- Android first; the bridge must be shaped so an iOS backend can slot in later.
- Only one WebView needs to be *visible* at a time, BUT see §2.1 — correctness still
  requires handle-routing because the app creates multiple `WebViewWrapper` instances
  over time (and sometimes concurrently).
- Help content ships inside the jar and is served via the existing `resource:/` scheme.
- JS execution needed (`executeScript`); JS→Java via the built-in `exportObject` bridge.

---

## 0. The moving parts (read this first)

```
EmbraceDesktop (jar, GraalVM native image "Graal side")
  PlatformProvider.getNewWebview()                     [Provided/…/PlatformProvider.java]
    └─ MobilePlatform.getNewWebview() → new AndroidWebView()   [DesktopMobile]
         └─ new javafx.scene.web.WebView()             ← Gluon JavaFX *Android* variant
              (from javafx static SDK, e.g. ~/.gluon/substrate/javafxStaticSdk/24-ea+7.1/
               android-aarch64/sdk/lib/javafx.web.jar — NOT the desktop WebKit WebView)
              natives: _initWebView, _setTransform, _setWidth, _setHeight,
                       _setVisible, _removeWebView; WebEngine: _loadUrl, _loadContent,
                       _reload, _executeScript
                  │ JNI (Graal side)
Substrate fork    ▼
  src/main/resources/native/android/c/bridge_webview.c   ← implements the natives above
  src/main/resources/native/android/c/javafx_adapter.c   ← #ifdef JAVAFX_WEB section;
      crosses from Graal JNI to the Dalvik VM (ATTACH_DALVIK/ATTACH_GRAAL macros,
      grandroid.h) and multiplies coordinates by `density`
                  │ Dalvik JNI
Android app       ▼
  android_project/…/NativeWebView.java   ← owns the android.webkit.WebView,
      adds it to MainActivity.mViewGroup (FrameLayout) with left/top margins.
      The native WebView renders ABOVE the JavaFX SurfaceView (hence the existing
      popup auto-hide workaround in AndroidWebView.java).
```

Build wiring (already works, keep as-is):
- `InternalProjectConfiguration.hasWeb()` → adds `-DJAVAFX_WEB` C flag
  (`AndroidTargetConfiguration.getTargetSpecificCCompileFlags`), links `-lwebview`
  whole-archive, and keeps `NativeWebView.java` in the copied android project
  (`prepareAndroidProject()` deletes it when web is absent).
- Version pipeline: substrate `gradle.properties` version → GluonfxMavenPlugin
  dependency bump → EmbraceDesktop `pom.xml` plugin version bump.

### 0.1 The JavaFX-side contract (decoded from javafx.web.jar 24-ea+7.1, android)

This is the obscure part nobody can see without decompiling. The Android
`javafx.scene.web.WebView` (bytecode-verified):

| Trigger (JavaFX application thread)                  | Native call |
|------------------------------------------------------|-------------|
| Constructor                                           | `_initWebView(long[1] nativeHandle)` — **must synchronously write a unique id into `nativeHandle[0]`**; the constructor stores it as `this.handle` and passes it to *every* later native call. Today our C code never writes it and ignores it on receipt — root of most bugs (§2.1). |
| `layoutBounds` change **and** any ancestor-transform change (`doTransformsChanged`) | `_setTransform(handle, mxx…mzt)` with the full node→scene affine from `calculateNodeToSceneTransform` (walks `NodeHelper.getLeafTransform` up the parent chain). Our bridge consumes only `mxt`(=scene x) and `myt`(=scene y). |
| `width`/`height` property change (node resize)        | `_setWidth(handle, w)` / `_setHeight(handle, h)` — logical (JavaFX) pixels. |
| `treeVisible` change; **and a stage pulse listener that runs EVERY frame**: visible ⇔ (treeVisible && scene != null && window showing), guarded by a cached `nativeVisible` boolean | `_setVisible(handle, boolean)` |
| Node removed from its parent                          | `_removeWebView(handle)` |
| `WebEngine.load/loadContent/reload/executeScript`     | `_loadUrl/_loadContent/_reload/_executeScript(handle, …)` |

Callbacks the JavaFX side expects (via `GetMethodID` on the `WebView` jobject):
`notifyLoadStarted()`, `notifyLoadFinished(String url, String innerHtml)`,
`notifyLoadFailed()`, `notifyJavaCall(String url)` — all marshalled back onto the
JavaFX thread by `checkThreadAndRun`.

JS→Java: `WebEngine.exportObject(String name, Object o)` exists on this port
(`JS2JavaBridge`): it injects a JS proxy (from an internal `helper_export_Object`
resource) after load-finished; page JS calls on the proxy navigate to a
`javacall:` URL, which Dalvik's `shouldOverrideUrlLoading` intercepts
(`url.contains("javacall")`) → `nativeJavaCallURL` → `notifyJavaCall` →
`JS2JavaBridge` decodes (own JSON encoder) and invokes the Java object.

---

## 1. What we keep from the stash

- The `resource:/` lazy-serving pipeline: `shouldInterceptRequest` →
  `loadResourceBytes` (Dalvik→C) → `ATTACH_GRAAL` →
  `AndroidWebViewEngine.getResourceBytes(path)` (Graal side classpath read).
  This is the right design (WebViewAssetLoader-style). Keep, but fix its JNI
  error handling and register the class for JNI (§3.4 — it is currently NOT in
  any jni-config, so `FindClass` fails at runtime in a native image and leaves a
  pending exception that poisons the Graal JNI env → part of the "random" behaviour).
- `getMimeTypeForUrl`.
- EmbraceDesktop's `AndroidWebView` popup auto-hide listener and the
  `StackPane.setAlignment(webView, Pos.TOP_LEFT)` fix. Both stay.
- WebSettings block (JS enabled, DOM storage, etc.).

## 2. Root causes of the "jumps / covers other content" bug — remove, don't patch

Delete all three band-aids in the stashed `NativeWebView.java` once the fixes below
are in: the 400 ms delayed refresh in `loadUrl`, the position refresh in
`onPageFinished`, and the refresh in `setVisible(true)`.

### 2.1 Singleton bridge vs. multiple JavaFX WebView instances (primary cause)

`bridge_webview.c` keeps ONE static `webViewObject`; `substrate_showWebView()`
*replaces* the one static Dalvik `nativeWebViewObj`. But EmbraceDesktop creates a
NEW `javafx.scene.web.WebView` every time `getNewWebview()` is called
(`EmbraceWebHelpWindow`, `JsonView`, `EmbraceLogForm` all do). Sequence that
produces exactly the reported symptom:

1. Help opens → WebView#1 → Dalvik view A added to `mViewGroup`.
2. Help closes & reopens (or another wrapper is built) → WebView#2 →
   `nativeWebViewObj` now points at Dalvik view B. **View A is still attached**,
   frozen at whatever position it last had (possibly mid-animation) — "webview
   half off the screen covering other content".
3. WebView#1's parent-null listener eventually fires `_removeWebView(handle₁)` —
   but the C side ignores the handle and removes **B**, the live one.
4. Any late `_setTransform` from #1 repositions **B** — "jumps to the wrong spot".

Timing of scene-graph removal vs. re-creation is nondeterministic → "race-like,
occasional". **Fix: route everything by the `handle` the JavaFX side already
threads through every call** (§3.1). This costs little — the Java side plumbing
already exists in the SDK — and incidentally makes future multi-instance
(helps + AI panel) nearly free.

### 2.2 Unsynchronized cross-thread geometry

`x/y/width/height` in `NativeWebView` are plain fields written on the Graal/JavaFX
thread (JNI upcall) and read on the Android UI thread inside posted runnables — no
volatile/lock. Stale or torn reads on ARM are legal outcomes. Also `setWidth`/
`setHeight` carry equality guards that skip updates, and `reLayout()`'s
`layoutStarted`/`inlayout` flags are mutated from mixed threads (double `addView`
possible). **Fix: single lock + coalesced apply (§3.2).**

### 2.3 JNI undefined behaviour (explains "random")

Concrete defects in the current C code — each can corrupt or silently kill later
JNI calls on that thread:

- `substrate_executeScript` (javafx_adapter.c) returns the pointer from
  `GetStringUTFChars` **after `DETACH_DALVIK()`** — use-after-detach; also never
  released (leak). The caller then `NewStringUTF`s from the dangling pointer.
- `androidJfx_finishURL` (bridge_webview.c) calls
  `ReleaseStringUTFChars(graalEnv, url, jurl)` with swapped/mistyped arguments
  (`url` is `const char*`, `jurl` is a `jstring`). Should be `DeleteLocalRef`.
- No `ExceptionCheck` after any `Call*Method`. One pending exception (e.g. the
  guaranteed-failing `FindClass` of `AndroidWebViewEngine`, §3.4) makes all
  subsequent JNI on that thread undefined → dropped position updates.
- `executeScript` Java side: `latch.await()` with no timeout; NPE if called
  before the UI-thread constructor runnable has assigned `webView`.

---

## 3. Implementation

Work in this order; each phase is independently buildable/testable.

### Phase 3.1 — Handle-routed bridge (substrate: C + Dalvik)

Files: `native/android/c/bridge_webview.c`, `native/android/c/bridge_webview.h`,
`native/android/c/javafx_adapter.c`,
`android_project/app/src/main/java/com/gluonhq/helloandroid/NativeWebView.java`.

**Design: the C layer stays dumb; the instance map lives on each Java side.**

C keeps a small handle table for the *Graal-side* callback objects, and calls
*static* router methods on the Dalvik `NativeWebView` class:

```c
/* bridge_webview.c */
#include <pthread.h>

#define MAX_WEBVIEWS 8
typedef struct {
    jlong   handle;      /* 0 = free slot */
    jobject fxWebView;   /* global ref, Graal heap: the javafx.scene.web.WebView */
} WebViewSlot;
static WebViewSlot slots[MAX_WEBVIEWS];
static jlong nextHandle = 1;
static pthread_mutex_t slotsMutex = PTHREAD_MUTEX_INITIALIZER;

static jlong registerWebView(JNIEnv *env, jobject fxWebView) {
    pthread_mutex_lock(&slotsMutex);
    jlong h = nextHandle++;
    for (int i = 0; i < MAX_WEBVIEWS; i++) {
        if (slots[i].handle == 0) {
            slots[i].handle = h;
            slots[i].fxWebView = (*env)->NewGlobalRef(env, fxWebView);
            pthread_mutex_unlock(&slotsMutex);
            return h;
        }
    }
    pthread_mutex_unlock(&slotsMutex);
    fprintf(stderr, "WebView: slot table full (>%d live WebViews)\n", MAX_WEBVIEWS);
    return 0;
}

static jobject fxWebViewForHandle(jlong h) { /* lock, linear scan, return jobject or NULL */ }
static void unregisterWebView(JNIEnv *env, jlong h) { /* lock, DeleteGlobalRef, zero slot */ }
```

`_initWebView` must write the handle back **synchronously** (the constructor reads
`nativeHandle[0]` immediately after return):

```c
JNIEXPORT void JNICALL
Java_javafx_scene_web_WebView__1initWebView(JNIEnv *env, jobject obj, jlongArray nativeHandle) {
    jlong handle = registerWebView(env, obj);
    (*env)->SetLongArrayRegion(env, nativeHandle, 0, 1, &handle);
    substrate_createWebView(handle);   /* was substrate_showWebView() */
}
```

Every `Java_javafx_scene_web_*` function already receives `jlong handle` — stop
ignoring it and pass it into the `substrate_*` functions, whose signatures all gain
a leading `jlong handle`.

`javafx_adapter.c` — method IDs become **static** with a leading `J`:

```c
/* registerJavaFXMethodHandles(): note class-static methods now */
nativeWebView_create  = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "create",      "(J)V");
nativeWebView_loadUrl = (*aenv)->GetStaticMethodID(aenv, nativeWebViewClass, "loadUrl",     "(JLjava/lang/String;)V");
/* …same pattern for loadContent(JLjava/lang/String;)V, setX(JD)V, setY(JD)V,
   setWidth(JD)V, setHeight(JD)V, setVisible(JZ)V, executeScript(JLjava/lang/String;)Ljava/lang/String;,
   reload(J)V, remove(J)V */
```

```c
void substrate_setWebViewX(jlong handle, double x) {
    ATTACH_DALVIK();
    (*dalvikEnv)->CallStaticVoidMethod(dalvikEnv, nativeWebViewClass, nativeWebView_x,
                                       handle, x * density);   /* density: logical→physical px, set in surfaceReady */
    checkAndClearException(dalvikEnv, "setWebViewX");
    DETACH_DALVIK();
}
```

Callbacks gain the handle too. Dalvik natives change to
`nativeStartURL(long handle, String url)` etc. (declared on `NativeWebView`,
implemented in javafx_adapter.c), and the Graal-side dispatch looks up the right
JavaFX object:

```c
void androidJfx_startURL(jlong handle, const char *url) {
    ATTACH_GRAAL();
    jobject fx = fxWebViewForHandle(handle);
    if (fx != NULL) {
        (*graalEnv)->CallVoidMethod(graalEnv, fx, jmidLoadStarted);
        checkAndClearException(graalEnv, "androidJfx_startURL");
    }
    DETACH_GRAAL();
}
```

`initializeWebViewHandles` no longer stores a single `webViewObject`; it only
resolves the four `jmid*` method IDs once (from the class of the first object) —
or better, resolve them from `FindClass("javafx/scene/web/WebView")` once.

`_removeWebView(handle)`: call `substrate_removeWebView(handle)` (Dalvik removes
and destroys its view, drops its map entry) **then** `unregisterWebView(graalEnv, handle)`.

### Phase 3.2 — Rewritten `NativeWebView.java` (geometry correctness)

Full replacement structure — the key patterns a subordinate must not "simplify away":

```java
public class NativeWebView {
    private static final String TAG = "GraalActivity";
    private static final java.util.concurrent.ConcurrentHashMap<Long, NativeWebView> INSTANCES =
            new java.util.concurrent.ConcurrentHashMap<>();

    // ---- static routers, called from C (any thread). Null-guard: events for a
    // ---- removed/unknown handle are DROPPED (this is correct; log at VERBOSE).
    public static void create(long handle)             { INSTANCES.put(handle, new NativeWebView(handle)); }
    public static void loadUrl(long handle, String u)  { NativeWebView i = INSTANCES.get(handle); if (i != null) i.doLoadUrl(u); }
    public static void setX(long handle, double v)     { NativeWebView i = INSTANCES.get(handle); if (i != null) i.updateGeom(v, null, null, null); }
    public static void setY(long handle, double v)     { NativeWebView i = INSTANCES.get(handle); if (i != null) i.updateGeom(null, v, null, null); }
    public static void setWidth(long handle, double v) { NativeWebView i = INSTANCES.get(handle); if (i != null) i.updateGeom(null, null, v, null); }
    public static void setHeight(long handle, double v){ NativeWebView i = INSTANCES.get(handle); if (i != null) i.updateGeom(null, null, null, v); }
    public static void setVisible(long handle, boolean v) { … }
    public static String executeScript(long handle, String s) { … }
    public static void reload(long handle) { … }
    public static void remove(long handle) { NativeWebView i = INSTANCES.remove(handle); if (i != null) i.doRemove(); }

    private final long handle;
    private final MainActivity activity = MainActivity.getInstance();
    private WebView webView;                 // UI thread only
    private boolean removed = false;         // UI thread only

    // ---- geometry: single source of truth, one lock, coalesced apply ----
    private final Object geomLock = new Object();
    private double gx, gy, gw, gh;                 // guarded by geomLock; PHYSICAL px (C multiplied by density)
    private boolean visible = true;                // guarded by geomLock
    private final java.util.concurrent.atomic.AtomicBoolean applyPending =
            new java.util.concurrent.atomic.AtomicBoolean(false);

    private void updateGeom(Double x, Double y, Double w, Double h) {
        synchronized (geomLock) {
            if (x != null) gx = x;  if (y != null) gy = y;
            if (w != null) gw = w;  if (h != null) gh = h;
        }
        scheduleApply();
    }

    private void scheduleApply() {
        // Coalesce: at most one apply queued. CRITICAL ordering inside
        // applyGeometry(): clear the flag BEFORE reading the values, so a setter
        // racing with the apply re-schedules rather than being lost.
        if (applyPending.compareAndSet(false, true)) {
            activity.runOnUiThread(this::applyGeometry);
        }
    }

    private void applyGeometry() {               // UI thread
        applyPending.set(false);                 // BEFORE the read — see above
        int lx, ly, lw, lh; boolean vis;
        synchronized (geomLock) {
            lx = (int) Math.round(gx); ly = (int) Math.round(gy);
            lw = (int) Math.round(gw); lh = (int) Math.round(gh);
            vis = visible;
        }
        if (removed || webView == null) return;
        if (lw <= 0 || lh <= 0) return;          // never attach a 0/WRAP_CONTENT-sized view
        FrameLayout.LayoutParams lp;
        if (webView.getParent() == null) {
            lp = new FrameLayout.LayoutParams(lw, lh, Gravity.NO_GRAVITY);
            lp.leftMargin = lx; lp.topMargin = ly;
            MainActivity.getViewGroup().addView(webView, lp);   // deferred attach: first non-zero geometry
        } else {
            lp = (FrameLayout.LayoutParams) webView.getLayoutParams();
            lp.leftMargin = lx; lp.topMargin = ly; lp.width = lw; lp.height = lh;
            MainActivity.getViewGroup().updateViewLayout(webView, lp);
        }
        webView.setVisibility(vis ? View.VISIBLE : View.GONE);
    }
```

Notes for the implementer:

- **Constructor** only stores `handle` and posts one UI runnable that builds the
  `android.webkit.WebView`, the `WebViewClient`/`WebChromeClient` (reuse the
  stash's client code, including `shouldInterceptRequest` for `resource:/` and the
  `javacall` interception in `shouldOverrideUrlLoading`), the `WebSettings` block —
  then calls `applyGeometry()` once. Do NOT attach in the constructor. Because
  `runOnUiThread` posts FIFO, the creation runnable always runs before any
  apply posted afterwards.
- **Visibility** goes through the same path: setter updates `visible` under
  `geomLock`, then `scheduleApply()`. No special "refresh position on show" code —
  the apply always writes the whole geometry. (JavaFX calls `_setVisible` from a
  per-frame pulse guarded by its `nativeVisible` cache, so this is not hot.)
- **remove()** (UI thread): `getViewGroup().removeView(webView)`,
  `webView.destroy()` (releases Chromium renderer — matters when help windows are
  opened/closed repeatedly), `removed = true`, `webView = null`.
- **executeScript**: keep the latch pattern but (a) if the UI runnable can't run
  because `webView` is not yet created, return `null` immediately; (b) use
  `latch.await(10, TimeUnit.SECONDS)` and log on timeout — never hang the JavaFX
  thread forever; (c) it is called on the JavaFX/Graal thread, never on the
  Android UI thread, so the latch cannot self-deadlock — assert with
  `Looper.myLooper() != Looper.getMainLooper()`.
- **Page callbacks** pass the handle back:
  `private native void nativeStartURL(long handle, String url);` etc., invoked with
  `this.handle`. Keep the `XMLSerializer` HTML capture in `onPageFinished` (the
  JavaFX `WebEngine` uses it to build its `Document`); if profiling later shows it
  is slow on large help pages, it can be replaced with `""` — but verify nothing
  in EmbraceDesktop reads `engine.getDocument()` first.
- `WebView.setWebContentsDebuggingEnabled(...)`: enable when the app is debuggable:
  `(activity.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0`
  → then `chrome://inspect` works on dev builds only.

### Phase 3.3 — JNI hygiene (C)

Add once in `bridge_webview.c` and `javafx_adapter.c` (or a shared header):

```c
static int checkAndClearException(JNIEnv *env, const char *where) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);   /* logs to logcat via stderr redirect */
        (*env)->ExceptionClear(env);
        fprintf(stderr, "WebView: JNI exception in %s (cleared)\n", where);
        return 1;
    }
    return 0;
}
```

Call it after EVERY `Call*Method`, `FindClass`, `GetMethodID` in the webview paths.

Fix `substrate_executeScript` (copy before detach, caller frees):

```c
char* substrate_executeScript(jlong handle, char* script) {
    ATTACH_DALVIK();
    jstring jscript = (*dalvikEnv)->NewStringUTF(dalvikEnv, script);
    jstring result = (jstring)(*dalvikEnv)->CallStaticObjectMethod(dalvikEnv,
            nativeWebViewClass, nativeWebView_executeScript, handle, jscript);
    char *copy = NULL;
    if (!checkAndClearException(dalvikEnv, "executeScript") && result != NULL) {
        const char *chars = (*dalvikEnv)->GetStringUTFChars(dalvikEnv, result, NULL);
        copy = strdup(chars);
        (*dalvikEnv)->ReleaseStringUTFChars(dalvikEnv, result, chars);
    }
    DETACH_DALVIK();
    return copy;
}

/* bridge_webview.c caller: */
JNIEXPORT jstring JNICALL Java_javafx_scene_web_WebEngine__1executeScript(JNIEnv *env, jobject cl, jlong handle, jstring script) {
    char *cscript = (char *)(*env)->GetStringUTFChars(env, script, JNI_FALSE);
    char *result = substrate_executeScript(handle, cscript);
    (*env)->ReleaseStringUTFChars(env, script, cscript);
    jstring jresult = (result != NULL) ? (*env)->NewStringUTF(env, result) : NULL;
    free(result);
    return jresult;
}
```

In `androidJfx_finishURL`, replace both `ReleaseStringUTFChars(graalEnv, url, jurl)`
calls with `(*graalEnv)->DeleteLocalRef(graalEnv, jurl)` / `…jhtml)`.

### Phase 3.4 — native-image config (currently missing → runtime failures)

The Graal-side `FindClass("za/co/embrace/desktop/utilities/provided/AndroidWebViewEngine")`
in `substrate_loadResourceBytes` requires a JNI registration in the native image.
It exists in **no** config file today (verified: nothing in
`DesktopAndroid/native-conf/*.json` nor substrate's `resources/config`). Add to the
jni config that the gluonfx build actually consumes (check the plugin's
`nativeImageArgs`/conf-dir wiring — there are both `jni-config.json` and
`jniconfig.json` in `native-conf`; confirm which one lands in
`-H:JNIConfigurationFiles`, or add to both):

```json
{
  "name": "za.co.embrace.desktop.utilities.provided.AndroidWebViewEngine",
  "methods": [ { "name": "getResourceBytes", "parameterTypes": ["java.lang.String"] } ]
}
```

Also confirm the help content is embedded: every file under the help tree must
match a pattern in `resourceconfig.json` / `resource-config.json`, e.g.

```json
{ "pattern": "za/co/embrace/desktop/windows/help/.*" }
```

(`getResourceAsStream` returns null in a native image for unregistered resources —
that would look like "help page blank", not an error.)

Verification step: the `javafx.scene.web.WebView.notifyLoadStarted/Finished/Failed/JavaCall`
methods are resolved from C via `GetMethodID` on the Graal env. The stash reached
these callbacks, so they are being retained today — but after the refactor, smoke-test
early; if `GetMethodID` returns NULL, add a `javafx.scene.web.WebView` entry (those
four methods) to the same jni config.

### Phase 3.5 — EmbraceDesktop side

- **No structural change required** for correctness once 3.1–3.4 land: creating a
  fresh `AndroidWebView` per help window is now safe. Still, prefer reusing one
  instance per surface (cheaper than re-spawning a Chromium renderer each open).
- **Loading helps**: `engine.load("resource:/za/co/embrace/desktop/…/index.html")`.
  Relative links and subresources inside the pages work automatically — Chromium
  resolves them against the `resource:/` base and each request funnels through
  `shouldInterceptRequest` → `getResourceBytes`.
- **JS→Java (help navigation → open program/screen)**: after the page loads, call
  `webEngine.exportObject("embraceHelp", helpBridge)` (cast: `MobileWebViewEngine`
  wraps the real `WebEngine`; add an `exportObject` passthrough to
  `WebviewEngineWrapper` with a no-op/JSObject impl on desktop, where the same is
  done via `window.setMember`). Page JS then calls `embraceHelp.navigate('PROG123')`;
  the call travels via a `javacall:` URL that `shouldOverrideUrlLoading` intercepts.
  **Prove this path with a hello-world early** (it's the least-exercised SDK code);
  fallback if `exportObject` misbehaves on this port: have page JS set
  `document.title = JSON.stringify(payload)` and listen to
  `engine.titleProperty()`, or navigate to a URL containing `javacall` and parse it
  yourself in a custom handler.
- **Java→JS (future AI panel pushes)**: `engine.executeScript("window.embraceOnEvent(" + json + ")")`.
  It is synchronous (JavaFX thread blocks on the Dalvik round-trip, bounded by the
  new 10 s timeout); for token streaming later, batch tokens (e.g. flush every
  50–100 ms) rather than one call per token.
- **Z-order constraint** (unchanged, document for UI devs): the native WebView is
  always ON TOP of everything JavaFX renders. JavaFX popups/menus/dialogs cannot
  overlap it — the existing `Window.getWindows()` auto-hide listener in
  `AndroidWebView` handles this; keep it. The mods bar etc. must simply not share
  screen area with the webview rectangle.
- Note: `_setTransform` scale components are ignored by the bridge — do not put the
  webview inside scaled/rotated containers (translation-only ancestors are fine).

### Phase 3.6 — Release plumbing

1. substrate: bump `gradle.properties` version; build & publish the jar the usual way.
2. GluonfxMavenPlugin: bump its substrate dependency to the new version; bump the
   plugin version.
3. EmbraceDesktop `pom.xml`: reference the new plugin version; rebuild the Android app.

---

## 4. Verification matrix (run on a physical device, incl. one high-DPI)

| # | Test | Passes when |
|---|------|-------------|
| 1 | Open help window | WebView appears at the designated rectangle immediately — no flash at 0,0, no 400 ms settle |
| 2 | Close/reopen help 10× rapidly | No orphaned overlay ever remains; each reopen positions correctly (primary regression for §2.1) |
| 3 | Animate/slide the containing panel | WebView tracks the panel; final position exact |
| 4 | Open a JavaFX context menu / popup over the webview | WebView hides, restores at correct position |
| 5 | Rotate device; show/hide soft keyboard | Correct geometry after each |
| 6 | Help page loads css/js/images via `resource:/` | All subresources served (watch logcat for `resource: not found`) |
| 7 | `exportObject` roundtrip: page button → Java callback; `executeScript` returns a value | Both directions work; no JNI exception lines in logcat |
| 8 | Two wrappers alive at once (open help while `JsonView`/log form exists, if reachable on mobile) | Each view independent; closing one leaves the other intact |
| 9 | Leak check: open/close help 20×, `adb shell dumpsys meminfo <pkg>` | No monotonic WebView/native growth (confirms `webView.destroy()` + global-ref cleanup) |
| 10 | Dev build: `chrome://inspect` attaches | Debugging available on debuggable builds only |

Logging: all bridge logs already tag `GraalActivity` or print `WebView,` to stderr →
`adb logcat | grep -E "GraalActivity|WebView"`.

---

## 5. iOS readiness (design notes only, no work now)

- The iOS peer is implemented *inside* the JavaFX static SDK (`libwebview.a`, WKWebView
  based); substrate only links it and the `WebKit` framework (already listed in
  `IosTargetConfiguration`). Nothing in this plan touches shared/iOS files — all
  changes live under `native/android/` — so iOS keeps building.
- Risk to log for later: Gluon's iOS native implementation may carry the same
  singleton assumption internally, and it is prebuilt (not patchable from this
  repo). Mitigation when iOS work starts: reuse a single `WebViewWrapper`
  app-wide on iOS, or rebuild the static SDK from Gluon's javafx fork with an
  equivalent handle fix.
- The `resource:/` interception has no iOS equivalent yet; the WKWebView impl would
  need a `WKURLSchemeHandler` — note as a future task.

## 6. Explicit non-goals / removed code

- The three timing hacks (400 ms delayed relayout, `onPageFinished` refresh,
  `setVisible(true)` refresh) — delete; they mask §2.1/§2.2 and cause visible snapping.
- `WRAP_CONTENT` initial attach in `reLayout()` — replaced by deferred attach.
- Multi-*visible* webview UX — out of scope (but works incidentally after 3.1).
