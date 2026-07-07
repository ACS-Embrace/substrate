/*
 * Copyright (c) 2020, 2023, Gluon
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
package com.gluonhq.helloandroid;

import android.content.pm.ApplicationInfo;
import android.graphics.Bitmap;
import android.os.Build;
import android.os.Looper;
import androidx.annotation.RequiresApi;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;

import java.io.ByteArrayInputStream;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Dalvik-side peer of one javafx.scene.web.WebView instance.
 *
 * Instances are keyed by the handle the JavaFX side generated in _initWebView
 * (bridge_webview.c) and threads through every native call. All entry points
 * from native code are static routers taking that handle; events for handles
 * that were already removed (or never created) are dropped — this is correct:
 * a JavaFX WebView being torn down can still emit transform/visibility events.
 *
 * Threading: the static routers are called on the JavaFX (Graal) thread. All
 * android.view / android.webkit access happens on the UI thread. Geometry is
 * kept in fields guarded by geomLock and applied by a single coalesced
 * UI-thread runnable, so out-of-order or dropped updates cannot occur and the
 * last write always wins.
 */
public class NativeWebView {

    private static final String TAG = "GraalActivity";

    private static final ConcurrentHashMap<Long, NativeWebView> INSTANCES = new ConcurrentHashMap<>();

    // ---- static routers, invoked from javafx_adapter.c ----

    public static void create(long handle) {
        Log.v(TAG, "NativeWebView create, handle " + handle);
        INSTANCES.put(handle, new NativeWebView(handle));
    }

    public static void loadUrl(long handle, String url) {
        NativeWebView i = INSTANCES.get(handle);
        if (i != null) i.doLoadUrl(url);
    }

    public static void loadContent(long handle, String content) {
        NativeWebView i = INSTANCES.get(handle);
        if (i != null) i.doLoadContent(content);
    }

    public static void setX(long handle, double x) {
        NativeWebView i = INSTANCES.get(handle);
        if (i != null) { synchronized (i.geomLock) { i.gx = x; } i.scheduleApply(); }
    }

    public static void setY(long handle, double y) {
        NativeWebView i = INSTANCES.get(handle);
        if (i != null) { synchronized (i.geomLock) { i.gy = y; } i.scheduleApply(); }
    }

    public static void setWidth(long handle, double w) {
        NativeWebView i = INSTANCES.get(handle);
        if (i != null) { synchronized (i.geomLock) { i.gw = w; } i.scheduleApply(); }
    }

    public static void setHeight(long handle, double h) {
        NativeWebView i = INSTANCES.get(handle);
        if (i != null) { synchronized (i.geomLock) { i.gh = h; } i.scheduleApply(); }
    }

    public static void setVisible(long handle, boolean visible) {
        NativeWebView i = INSTANCES.get(handle);
        if (i != null) { synchronized (i.geomLock) { i.gVisible = visible; } i.scheduleApply(); }
    }

    public static String executeScript(long handle, String script) {
        NativeWebView i = INSTANCES.get(handle);
        return i != null ? i.doExecuteScript(script) : null;
    }

    public static void reload(long handle) {
        NativeWebView i = INSTANCES.get(handle);
        if (i != null) i.doReload();
    }

    public static void remove(long handle) {
        Log.v(TAG, "NativeWebView remove, handle " + handle);
        NativeWebView i = INSTANCES.remove(handle);
        if (i != null) i.doRemove();
    }

    // ---- instance ----

    private final long handle;
    private final MainActivity activity;

    // UI thread only:
    private WebView webView;
    private boolean removed = false;

    // Geometry in physical pixels (density scaling already applied in C).
    private final Object geomLock = new Object();
    private double gx, gy, gw, gh;   // guarded by geomLock
    private boolean gVisible = true; // guarded by geomLock

    private final AtomicBoolean applyPending = new AtomicBoolean(false);

    private NativeWebView(long handle) {
        this.handle = handle;
        this.activity = MainActivity.getInstance();
        // runOnUiThread is FIFO: this creation runnable always runs before any
        // applyGeometry posted by the setters afterwards.
        activity.runOnUiThread(this::createWebView);
    }

    private void createWebView() {
        webView = new WebView(activity);
        webView.setWebChromeClient(new WebChromeClient());
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                super.onPageStarted(view, url, favicon);
                Log.v(TAG, "Page started: " + url);
                nativeStartURL(handle, url);
            }

            @Override
            public void onPageFinished(WebView view, final String url) {
                Log.v(TAG, "Page finished: " + url);
                // The JavaFX WebEngine builds its Document from the serialized DOM.
                view.evaluateJavascript("new XMLSerializer().serializeToString(document)", new ValueCallback<String>() {
                    @Override
                    public void onReceiveValue(String s) {
                        nativeFinishURL(handle, url, unwrapJsResult(s));
                    }
                });
            }

            @Override
            public boolean shouldOverrideUrlLoading(WebView view, String url) {
                if (url.contains("javacall")) {
                    Log.v(TAG, "Stop url loading, due to javacall: " + url);
                    nativeJavaCallURL(handle, url);
                    return true;
                }
                return false;
            }

            @RequiresApi(api = Build.VERSION_CODES.M)
            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                Log.v(TAG, "LOAD onReceivedError: " + request.getUrl() + ": " + error.getDescription());
                nativeFailedURL(handle, request.getUrl().toString());
            }

            @Override
            public void onReceivedHttpError(WebView view, WebResourceRequest request, WebResourceResponse errorResponse) {
                Log.v(TAG, "LOAD onReceivedHttpError: " + request.getUrl() + ": " + errorResponse.getReasonPhrase());
                nativeFailedURL(handle, request.getUrl().toString());
            }

            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                String url = request.getUrl().toString();
                if (url.startsWith("resource:/")) {
                    String path = url.substring("resource:".length());
                    byte[] data = loadResourceBytes(path);
                    if (data != null) {
                        Log.v(TAG, "Serving resource lazily: " + url);
                        return new WebResourceResponse(getMimeTypeForUrl(url), "UTF-8", new ByteArrayInputStream(data));
                    }
                    Log.w(TAG, "resource: not found: " + url);
                }
                return super.shouldInterceptRequest(view, request);
            }
        });

        WebSettings webSettings = webView.getSettings();
        webSettings.setJavaScriptEnabled(true);
        webSettings.setJavaScriptCanOpenWindowsAutomatically(true);
        webSettings.setDomStorageEnabled(true);
        webSettings.setUseWideViewPort(true);
        webSettings.setLoadWithOverviewMode(true);
        webSettings.setAllowContentAccess(true);
        webSettings.setAllowFileAccess(true);
        webSettings.setBuiltInZoomControls(true);
        webSettings.setDisplayZoomControls(false);

        // chrome://inspect works on debuggable builds only.
        boolean debuggable = (activity.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        WebView.setWebContentsDebuggingEnabled(debuggable);

        applyGeometry();
        Log.v(TAG, "NativeWebView created, handle " + handle);
    }

    private void scheduleApply() {
        // Coalesce: at most one apply queued at any time.
        if (applyPending.compareAndSet(false, true)) {
            activity.runOnUiThread(this::applyGeometry);
        }
    }

    private void applyGeometry() {
        // Clear the flag BEFORE reading the values: a setter racing with this
        // apply then re-schedules instead of being lost.
        applyPending.set(false);
        int x, y, w, h;
        boolean visible;
        synchronized (geomLock) {
            x = (int) Math.round(gx);
            y = (int) Math.round(gy);
            w = (int) Math.round(gw);
            h = (int) Math.round(gh);
            visible = gVisible;
        }
        if (removed || webView == null) {
            return;
        }
        if (w <= 0 || h <= 0) {
            // Never attach (or keep showing) a zero-sized view: an early
            // WRAP_CONTENT attach is what caused the full-size flash at 0,0.
            if (webView.getParent() != null) {
                webView.setVisibility(View.GONE);
            }
            return;
        }
        if (webView.getParent() == null) {
            FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(w, h, Gravity.NO_GRAVITY);
            layout.leftMargin = x;
            layout.topMargin = y;
            MainActivity.getViewGroup().addView(webView, layout);
        } else {
            FrameLayout.LayoutParams layout = (FrameLayout.LayoutParams) webView.getLayoutParams();
            layout.leftMargin = x;
            layout.topMargin = y;
            layout.width = w;
            layout.height = h;
            MainActivity.getViewGroup().updateViewLayout(webView, layout);
        }
        webView.setVisibility(visible ? View.VISIBLE : View.GONE);
        Log.v(TAG, "applyGeometry handle=" + handle + " x=" + x + " y=" + y + " w=" + w + " h=" + h + " visible=" + visible);
    }

    private void doLoadUrl(final String url) {
        Log.v(TAG, "loadUrl, handle " + handle + ", url " + url);
        activity.runOnUiThread(() -> {
            if (!removed && webView != null) {
                webView.loadUrl(url);
            }
        });
    }

    private void doLoadContent(final String content) {
        Log.v(TAG, "loadContent, handle " + handle);
        activity.runOnUiThread(() -> {
            if (!removed && webView != null) {
                webView.loadDataWithBaseURL(null, content, "text/html", "UTF-8", null);
            }
        });
    }

    private void doReload() {
        activity.runOnUiThread(() -> {
            if (!removed && webView != null) {
                webView.reload();
            }
        });
    }

    private void doRemove() {
        activity.runOnUiThread(() -> {
            if (removed) {
                return;
            }
            removed = true;
            if (webView != null) {
                if (webView.getParent() != null) {
                    MainActivity.getViewGroup().removeView(webView);
                }
                // Release the Chromium renderer; help windows are opened and
                // closed repeatedly and each WebView holds native memory.
                webView.destroy();
                webView = null;
            }
        });
    }

    /**
     * Called on the JavaFX (Graal) thread, which blocks until the UI thread has
     * evaluated the script. Bounded wait: a page that never answers (nothing
     * loaded yet, renderer gone) must not hang the JavaFX thread forever.
     */
    private String doExecuteScript(final String script) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            // Would deadlock: the latch below is released by UI-thread work.
            Log.e(TAG, "executeScript called on the UI thread; returning null");
            return null;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        final AtomicReference<String> result = new AtomicReference<>();
        activity.runOnUiThread(() -> {
            if (removed || webView == null) {
                latch.countDown();
                return;
            }
            webView.evaluateJavascript(script, s -> {
                result.set(unwrapJsResult(s));
                latch.countDown();
            });
        });
        try {
            if (!latch.await(10, TimeUnit.SECONDS)) {
                Log.w(TAG, "executeScript timed out, handle " + handle);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return result.get();
    }

    /**
     * evaluateJavascript returns the result as a JSON value ("abc" arrives as
     * \"abc\" with inner escapes). The JavaFX WebEngine expects the raw string
     * its bridge script returned, so exactly one JSON layer is stripped here.
     */
    private static String unwrapJsResult(String s) {
        if (s == null || "null".equals(s)) {
            return null;
        }
        try {
            Object value = new JSONTokener(s).nextValue();
            if (value == null || value == JSONObject.NULL) {
                return null;
            }
            return value.toString();
        } catch (JSONException e) {
            return s;
        }
    }

    private static String getMimeTypeForUrl(String url) {
        if (url.endsWith(".html") || url.endsWith(".htm")) return "text/html";
        if (url.endsWith(".css")) return "text/css";
        if (url.endsWith(".js")) return "application/javascript";
        if (url.endsWith(".png")) return "image/png";
        if (url.endsWith(".jpg") || url.endsWith(".jpeg")) return "image/jpeg";
        if (url.endsWith(".svg")) return "image/svg+xml";
        if (url.endsWith(".gif")) return "image/gif";
        if (url.endsWith(".woff")) return "font/woff";
        if (url.endsWith(".woff2")) return "font/woff2";
        if (url.endsWith(".ico")) return "image/x-icon";
        if (url.endsWith(".json")) return "application/json";
        return "application/octet-stream";
    }

    private native byte[] loadResourceBytes(String path);

    private native void nativeStartURL(long handle, String url);
    private native void nativeFinishURL(long handle, String url, String innerHTML);
    private native void nativeFailedURL(long handle, String url);
    private native void nativeJavaCallURL(long handle, String url);
}
