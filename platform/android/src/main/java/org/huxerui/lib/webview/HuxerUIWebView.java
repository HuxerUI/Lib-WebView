package org.huxerui.lib.webview;

import android.content.Context;
import android.graphics.Bitmap;
import android.net.http.SslError;
import android.os.Build;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.JavascriptInterface;
import android.webkit.SslErrorHandler;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformView;
import org.huxerui.PlatformPayload;

import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import java.nio.charset.StandardCharsets;

public final class HuxerUIWebView implements HuxerUIPlatformView {
    private static final Set<String> PROPERTY_FIELDS = new LinkedHashSet<>(
            Arrays.asList("javaScriptEnabled", "url")
    );
    private static final Set<String> REQUEST_FIELDS = new LinkedHashSet<>(
            Arrays.asList("body", "headers", "method", "url")
    );
    private static final Set<String> HTML_FIELDS = new LinkedHashSet<>(
            Arrays.asList("baseUrl", "content")
    );

    private static final class Properties {
        final String url;
        final boolean javaScriptEnabled;

        Properties(String url, boolean javaScriptEnabled) {
            this.url = url;
            this.javaScriptEnabled = javaScriptEnabled;
        }

        static Properties decode(PlatformPayload payload) {
            payload.rejectUnknownFields(PROPERTY_FIELDS);
            return new Properties(
                    payload.requireField("url").requireString(),
                    payload.requireField("javaScriptEnabled").requireBoolean()
            );
        }
    }

    private static final class Request {
        final String url;
        final String method;
        final Map<String, String> headers;
        final String body;

        Request(String url, String method, Map<String, String> headers, String body) {
            this.url = url;
            this.method = method;
            this.headers = headers;
            this.body = body;
        }

        static Request decode(PlatformPayload payload) {
            payload.rejectUnknownFields(REQUEST_FIELDS);
            Map<String, String> headers = new LinkedHashMap<>();
            for (Map.Entry<String, PlatformPayload> entry
                    : payload.requireField("headers").fields().entrySet()) {
                headers.put(entry.getKey(), entry.getValue().requireString());
            }
            PlatformPayload body = payload.requireField("body");
            return new Request(
                    payload.requireField("url").requireString(),
                    payload.requireField("method").requireString(),
                    headers,
                    body.isNull() ? null : body.requireString()
            );
        }
    }

    private static final class Html {
        final String content;
        final String baseUrl;

        Html(String content, String baseUrl) {
            this.content = content;
            this.baseUrl = baseUrl;
        }

        static Html decode(PlatformPayload payload) {
            payload.rejectUnknownFields(HTML_FIELDS);
            return new Html(
                    payload.requireField("content").requireString(),
                    payload.requireField("baseUrl").requireString()
            );
        }
    }

    public static final class Factory implements HuxerUIPlatformView.Factory {
        public Factory() {}

        @Override
        public HuxerUIPlatformView create(
                Context context,
                PlatformPayload properties,
                HuxerUIPlatformChannel.Events events
        ) {
            return new HuxerUIWebView(context, Properties.decode(properties), events);
        }
    }

    private final WebView webView;
    private final HuxerUIPlatformChannel.Events events;
    private Properties properties;
    private boolean disposed;
    private boolean loading;

    private HuxerUIWebView(
            Context context,
            Properties properties,
            HuxerUIPlatformChannel.Events events
    ) {
        this.events = events;
        this.properties = properties;
        webView = new WebView(context);
        configureWebView();
        applyProperties(properties, true);
    }

    private void configureWebView() {
        WebSettings settings = webView.getSettings();
        settings.setAllowContentAccess(false);
        settings.setAllowFileAccess(false);
        settings.setJavaScriptCanOpenWindowsAutomatically(false);
        settings.setSupportMultipleWindows(false);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            settings.setSafeBrowsingEnabled(true);
        }

        webView.setWebViewClient(new Client());
        webView.setWebChromeClient(new ChromeClient());
        webView.addJavascriptInterface(new Bridge(), "huxerui");
    }

    private void applyProperties(Properties next, boolean initial) {
        boolean urlChanged = initial || !properties.url.equals(next.url);
        properties = next;
        webView.getSettings().setJavaScriptEnabled(next.javaScriptEnabled);
        if (urlChanged) {
            String url = next.url;
            webView.post(() -> {
                if (!disposed && properties.url.equals(url) && allowNavigation(url, true)) {
                    webView.loadUrl(url);
                }
            });
        }
    }

    private boolean allowNavigation(String url, boolean isMainFrame) {
        Map<String, PlatformPayload> fields = new LinkedHashMap<>();
        fields.put("isMainFrame", PlatformPayload.booleanValue(isMainFrame));
        fields.put("url", PlatformPayload.string(url == null ? "" : url));
        PlatformPayload result = events.emit("navigationRequested", PlatformPayload.object(fields));
        return result == null || result.isNull() || result.requireBoolean();
    }

    private PlatformPayload navigationState() {
        Map<String, PlatformPayload> fields = new LinkedHashMap<>();
        fields.put("canGoBack", PlatformPayload.booleanValue(webView.canGoBack()));
        fields.put("canGoForward", PlatformPayload.booleanValue(webView.canGoForward()));
        fields.put("isLoading", PlatformPayload.booleanValue(loading));
        fields.put("progress", PlatformPayload.doubleValue(webView.getProgress() / 100.0));
        fields.put("title", PlatformPayload.string(webView.getTitle() == null ? "" : webView.getTitle()));
        fields.put("url", PlatformPayload.string(webView.getUrl() == null ? properties.url : webView.getUrl()));
        return PlatformPayload.object(fields);
    }

    private void emitNavigationState() {
        if (disposed) {
            return;
        }
        events.emit("navigationChanged", navigationState());
    }

    private void emitStateEvent(String name) {
        if (!disposed) {
            events.emit(name, navigationState());
        }
    }

    private void emitLoadError(String code, String message, String url) {
        if (disposed) {
            return;
        }
        Map<String, PlatformPayload> fields = new LinkedHashMap<>();
        fields.put("code", PlatformPayload.string(code));
        fields.put("message", PlatformPayload.string(message == null ? "" : message));
        fields.put("url", url == null ? PlatformPayload.nullValue() : PlatformPayload.string(url));
        events.emit("loadFailed", PlatformPayload.object(fields));
    }

    @Override
    public WebView getView() {
        return webView;
    }

    @Override
    public void update(PlatformPayload payload) {
        if (!disposed) {
            applyProperties(Properties.decode(payload), false);
        }
    }

    @Override
    public HuxerUIPlatformChannel.Cancellation invoke(
            String method,
            PlatformPayload arguments,
            HuxerUIPlatformChannel.Result result
    ) {
        if (disposed) {
            result.fail("webview/disconnected", "The WebView is disconnected", PlatformPayload.nullValue());
            return null;
        }
        switch (method) {
            case "reload":
                arguments.requireNull();
                webView.reload();
                break;
            case "stopLoading":
                arguments.requireNull();
                webView.stopLoading();
                break;
            case "goBack":
                arguments.requireNull();
                if (webView.canGoBack()) {
                    webView.goBack();
                }
                break;
            case "goForward":
                arguments.requireNull();
                if (webView.canGoForward()) {
                    webView.goForward();
                }
                break;
            case "loadRequest":
                if (!loadRequest(Request.decode(arguments), result)) {
                    return null;
                }
                break;
            case "loadHtml":
                loadHtml(Html.decode(arguments));
                break;
            case "evaluateJavaScript":
                webView.evaluateJavascript(arguments.requireString(), value -> {
                    if (disposed) {
                        result.fail(
                                "webview/disconnected",
                                "The WebView was disconnected before JavaScript completed",
                                PlatformPayload.nullValue()
                        );
                    } else {
                        result.complete(PlatformPayload.string(value == null ? "null" : value));
                    }
                });
                return null;
            default:
                result.fail(
                        "webview/unknown-method",
                        "Unknown WebView method: " + method,
                        PlatformPayload.nullValue()
                );
                return null;
        }
        result.complete(PlatformPayload.nullValue());
        return null;
    }

    private boolean loadRequest(Request request, HuxerUIPlatformChannel.Result result) {
        String method = request.method.toUpperCase(java.util.Locale.ROOT);
        if ("GET".equals(method) && request.body == null) {
            if (allowNavigation(request.url, true)) {
                webView.loadUrl(request.url, request.headers);
            }
            return true;
        }
        if ("POST".equals(method) && request.headers.isEmpty()) {
            if (allowNavigation(request.url, true)) {
                byte[] body = request.body == null
                        ? new byte[0]
                        : request.body.getBytes(StandardCharsets.UTF_8);
                webView.postUrl(request.url, body);
            }
            return true;
        }
        result.fail(
                "webview/unsupported-request",
                "Android WebView supports GET headers or a POST body, but not this request combination",
                PlatformPayload.nullValue()
        );
        return false;
    }

    private void loadHtml(Html html) {
        String baseUrl = html.baseUrl.isEmpty() ? null : html.baseUrl;
        String navigationUrl = baseUrl == null ? "about:blank" : baseUrl;
        if (allowNavigation(navigationUrl, true)) {
            webView.loadDataWithBaseURL(baseUrl, html.content, "text/html", "UTF-8", null);
        }
    }

    @Override
    public void dispose() {
        if (disposed) {
            return;
        }
        disposed = true;
        webView.stopLoading();
        webView.removeJavascriptInterface("huxerui");
        webView.setWebChromeClient(null);
        webView.setWebViewClient(null);
        webView.loadUrl("about:blank");
        webView.clearHistory();
        webView.removeAllViews();
        webView.destroy();
    }

    private final class Client extends WebViewClient {
        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            return !allowNavigation(request.getUrl().toString(), request.isForMainFrame());
        }

        @SuppressWarnings("deprecation")
        @Override
        public boolean shouldOverrideUrlLoading(WebView view, String url) {
            return !allowNavigation(url, true);
        }

        @Override
        public void onPageStarted(WebView view, String url, Bitmap favicon) {
            properties = new Properties(url == null ? "" : url, properties.javaScriptEnabled);
            loading = true;
            emitStateEvent("loadStarted");
            emitNavigationState();
        }

        @Override
        public void onPageFinished(WebView view, String url) {
            loading = false;
            emitStateEvent("loadFinished");
            emitNavigationState();
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
            if (request.isForMainFrame()) {
                loading = false;
                emitLoadError(
                        "android/webview/" + error.getErrorCode(),
                        String.valueOf(error.getDescription()),
                        request.getUrl().toString()
                );
                emitNavigationState();
            }
        }

        @Override
        public void onReceivedSslError(WebView view, SslErrorHandler handler, SslError error) {
            handler.cancel();
            loading = false;
            emitLoadError(
                    "android/webview/ssl-" + error.getPrimaryError(),
                    "The WebView rejected the server certificate",
                    error.getUrl()
            );
            emitNavigationState();
        }

        @Override
        public boolean onRenderProcessGone(WebView view, RenderProcessGoneDetail detail) {
            loading = false;
            emitLoadError(
                    "android/webview/render-process-gone",
                    detail.didCrash() ? "The WebView render process crashed" : "The WebView render process exited",
                    view.getUrl()
            );
            return true;
        }
    }

    private final class ChromeClient extends WebChromeClient {
        @Override
        public void onProgressChanged(WebView view, int newProgress) {
            emitNavigationState();
        }

        @Override
        public void onReceivedTitle(WebView view, String title) {
            emitNavigationState();
        }
    }

    private final class Bridge {
        @JavascriptInterface
        public void postMessage(String message) {
            webView.post(() -> {
                if (!disposed) {
                    events.emit("messageReceived", PlatformPayload.string(message == null ? "" : message));
                }
            });
        }
    }
}
