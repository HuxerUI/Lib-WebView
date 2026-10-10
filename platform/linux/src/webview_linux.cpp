#include <webkit/webkit.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <huxerui/linux/platform_registry.h>
#include <huxerui/webview.h>

#include "detail/webview_internal.h"

namespace huxerui::detail {
namespace {

struct LinuxWebViewInstance : std::enable_shared_from_this<LinuxWebViewInstance> {
  WebKitWebView* web_view = nullptr;
  WebViewProperties properties;
  PlatformEventEmitter events;
  bool loading = false;
  bool load_failed = false;
  bool disposed = false;
};

std::string CurrentUrl(const LinuxWebViewInstance& instance) {
  const char* uri = instance.web_view != nullptr ? webkit_web_view_get_uri(instance.web_view) : nullptr;
  return uri != nullptr ? uri : instance.properties.url;
}

WebViewNavigationState NavigationState(const LinuxWebViewInstance& instance) {
  WebKitWebView* view = instance.web_view;
  const double progress = view != nullptr ? webkit_web_view_get_estimated_load_progress(view) : 0.0;
  return {
      .url = CurrentUrl(instance),
      .title = view != nullptr && webkit_web_view_get_title(view) != nullptr ? webkit_web_view_get_title(view) : "",
      .is_loading = instance.loading,
      .progress = std::clamp(progress, 0.0, 1.0),
      .can_go_back = view != nullptr && webkit_web_view_can_go_back(view),
      .can_go_forward = view != nullptr && webkit_web_view_can_go_forward(view),
  };
}

template <class Event> void EmitNavigationState(const std::shared_ptr<LinuxWebViewInstance>& instance) {
  if (instance && !instance->disposed && instance->web_view != nullptr) {
    instance->events.Emit<Event>(NavigationState(*instance));
  }
}

void EmitLoadError(const std::shared_ptr<LinuxWebViewInstance>& instance, std::string code, std::string message,
                   std::optional<std::string> uri = std::nullopt) {
  if (!instance || instance->disposed) {
    return;
  }
  instance->events.Emit<WebViewEvents::LoadFailed>(WebViewLoadError{
      .code = std::move(code),
      .message = std::move(message),
      .url = uri.has_value() ? std::move(uri) : std::optional<std::string>{CurrentUrl(*instance)},
  });
}

void LoadUrl(const std::shared_ptr<LinuxWebViewInstance>& instance, const std::string& url) {
  if (url.empty()) {
    return;
  }
  webkit_web_view_load_uri(instance->web_view, url.c_str());
}

void LoadRequest(const std::shared_ptr<LinuxWebViewInstance>& instance, const WebViewRequest& value) {
  std::string method = value.method;
  std::transform(method.begin(), method.end(), method.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  // WebKitGTK's public WebKitURIRequest exposes URI and headers, but not a setter for method or body.
  if (method != "GET" || value.body.has_value()) {
    EmitLoadError(instance, "linux/webview/request-capability",
                  "WebKitGTK 6.0 supports GET navigation requests only", value.url);
    return;
  }
  WebKitURIRequest* request = webkit_uri_request_new(value.url.c_str());
  if (request == nullptr) {
    EmitLoadError(instance, "linux/webview/invalid-request", "Could not create the WebKitGTK request", value.url);
    return;
  }
  SoupMessageHeaders* headers = webkit_uri_request_get_http_headers(request);
  for (const auto& [name, content] : value.headers) {
    soup_message_headers_replace(headers, name.c_str(), content.c_str());
  }
  webkit_web_view_load_request(instance->web_view, request);
  g_object_unref(request);
}

void LoadHtml(const std::shared_ptr<LinuxWebViewInstance>& instance, const WebViewHtml& html) {
  if (!html.base_url.empty() && !g_uri_is_valid(html.base_url.c_str(), G_URI_FLAGS_NONE, nullptr)) {
    EmitLoadError(instance, "linux/webview/invalid-base-url", "The WebView base URL is invalid", html.base_url);
    return;
  }
  webkit_web_view_load_html(instance->web_view, html.content.c_str(),
                            html.base_url.empty() ? nullptr : html.base_url.c_str());
}

struct JavaScriptCall {
  WebViewJavaScriptCompletion completion;
};

void JavaScriptFinished(GObject* source, GAsyncResult* result, gpointer data) {
  std::unique_ptr<JavaScriptCall> call(static_cast<JavaScriptCall*>(data));
  GError* error = nullptr;
  JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
  if (error != nullptr) {
    call->completion(PlatformError{"webview/javascript", error->message != nullptr ? error->message : "JavaScript failed", {}});
    g_error_free(error);
    return;
  }
  if (value == nullptr || jsc_value_is_undefined(value)) {
    if (value != nullptr) {
      g_object_unref(value);
    }
    call->completion(std::string("null"));
    return;
  }
  gchar* json = jsc_value_to_json(value, 0);
  g_object_unref(value);
  if (json == nullptr) {
    call->completion(PlatformError{"webview/javascript-result", "The JavaScript result is not JSON serializable", {}});
    return;
  }
  call->completion(std::string(json));
  g_free(json);
}

void EvaluateJavaScript(const std::shared_ptr<LinuxWebViewInstance>& instance,
                        WebViewEvaluateJavaScriptCommand command) {
  if (!instance || instance->disposed || instance->web_view == nullptr) {
    command.completion(PlatformError{"webview/disconnected", "The WebView is disconnected", {}});
    return;
  }
  auto* call = new JavaScriptCall{std::move(command.completion)};
  webkit_web_view_evaluate_javascript(instance->web_view, command.script.c_str(),
                                      static_cast<gssize>(command.script.size()), nullptr, nullptr, nullptr,
                                      JavaScriptFinished, call);
}

struct CookieOperation {
  explicit CookieOperation(WebKitCookieManager* manager_value, WebViewJavaScriptCompletion completion_value)
      : manager(WEBKIT_COOKIE_MANAGER(g_object_ref(manager_value))), completion(std::move(completion_value)) {}
  ~CookieOperation() { g_object_unref(manager); }

  WebKitCookieManager* manager;
  WebViewJavaScriptCompletion completion;
};

bool ValidCookieUri(const std::string& value) {
  GError* error = nullptr;
  GUri* uri = g_uri_parse(value.c_str(), G_URI_FLAGS_NONE, &error);
  const char* scheme = uri != nullptr ? g_uri_get_scheme(uri) : nullptr;
  const char* host = uri != nullptr ? g_uri_get_host(uri) : nullptr;
  const bool valid = uri != nullptr && scheme != nullptr && host != nullptr &&
                     (g_ascii_strcasecmp(scheme, "http") == 0 || g_ascii_strcasecmp(scheme, "https") == 0);
  if (uri != nullptr) {
    g_uri_unref(uri);
  }
  if (error != nullptr) {
    g_error_free(error);
  }
  return valid;
}

void CookiesReady(GObject* source, GAsyncResult* result, gpointer data) {
  std::unique_ptr<CookieOperation> operation(static_cast<CookieOperation*>(data));
  GError* error = nullptr;
  GList* cookies = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
  if (error != nullptr) {
    operation->completion(PlatformError{"webview/cookies", error->message != nullptr ? error->message : "Cookie query failed", {}});
    g_error_free(error);
    return;
  }
  GSList* cookie_list = nullptr;
  for (GList* item = cookies; item != nullptr; item = item->next) {
    cookie_list = g_slist_prepend(cookie_list, item->data);
  }
  cookie_list = g_slist_reverse(cookie_list);
  gchar* header = soup_cookies_to_cookie_header(cookie_list);
  g_slist_free(cookie_list);
  g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
  operation->completion(std::string(header != nullptr ? header : ""));
  g_free(header);
}

struct CookieDeleteBatch {
  explicit CookieDeleteBatch(WebKitCookieManager* manager_value, WebViewJavaScriptCompletion completion_value)
      : manager(WEBKIT_COOKIE_MANAGER(g_object_ref(manager_value))), completion(std::move(completion_value)) {}
  ~CookieDeleteBatch() { g_object_unref(manager); }

  WebKitCookieManager* manager;
  WebViewJavaScriptCompletion completion;
  std::size_t pending = 0;
  bool failed = false;
};

struct CookieDeleteItem {
  std::shared_ptr<CookieDeleteBatch> batch;
  SoupCookie* cookie = nullptr;
};

void CookieDeleted(GObject* source, GAsyncResult* result, gpointer data) {
  std::unique_ptr<CookieDeleteItem> item(static_cast<CookieDeleteItem*>(data));
  GError* error = nullptr;
  const gboolean deleted = webkit_cookie_manager_delete_cookie_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
  item->batch->failed = item->batch->failed || deleted == FALSE || error != nullptr;
  if (error != nullptr) {
    g_error_free(error);
  }
  soup_cookie_free(item->cookie);
  if (--item->batch->pending == 0) {
    if (item->batch->failed) {
      item->batch->completion(PlatformError{"webview/cookies-clear", "Could not clear every WebKitGTK cookie", {}});
    } else {
      item->batch->completion(std::string{});
    }
  }
}

void CookiesListedForClear(GObject* source, GAsyncResult* result, gpointer data) {
  std::unique_ptr<std::shared_ptr<CookieDeleteBatch>> holder(
      static_cast<std::shared_ptr<CookieDeleteBatch>*>(data));
  const std::shared_ptr<CookieDeleteBatch> batch = *holder;
  GError* error = nullptr;
  GList* cookies = webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
  if (error != nullptr) {
    batch->completion(PlatformError{"webview/cookies-clear", error->message != nullptr ? error->message : "Cookie clear failed", {}});
    g_error_free(error);
    return;
  }
  batch->pending = static_cast<std::size_t>(g_list_length(cookies));
  if (batch->pending == 0) {
    batch->completion(std::string{});
    return;
  }
  for (GList* item = cookies; item != nullptr; item = item->next) {
    auto* deletion = new CookieDeleteItem{batch, soup_cookie_copy(static_cast<SoupCookie*>(item->data))};
    webkit_cookie_manager_delete_cookie(batch->manager, deletion->cookie, nullptr, CookieDeleted, deletion);
  }
  g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
}

void RunCookieCommand(const std::shared_ptr<LinuxWebViewInstance>& instance, WebViewCookieCommand command) {
  if (!instance || instance->disposed || instance->web_view == nullptr) {
    command.completion(PlatformError{"webview/disconnected", "The WebView is disconnected", {}});
    return;
  }
  WebKitNetworkSession* session = webkit_web_view_get_network_session(instance->web_view);
  WebKitCookieManager* manager = session != nullptr ? webkit_network_session_get_cookie_manager(session) : nullptr;
  if (manager == nullptr) {
    command.completion(PlatformError{"webview/cookies", "The WebKitGTK cookie store is unavailable", {}});
    return;
  }
  if (command.clear) {
    auto batch = std::make_shared<CookieDeleteBatch>(manager, std::move(command.completion));
    webkit_cookie_manager_get_all_cookies(manager, nullptr, CookiesListedForClear,
                                           new std::shared_ptr<CookieDeleteBatch>(std::move(batch)));
    return;
  }
  if (!ValidCookieUri(command.url)) {
    command.completion(PlatformError{"webview/invalid-url", "Cookie URL must be an absolute HTTP or HTTPS URL", {}});
    return;
  }
  auto* operation = new CookieOperation(manager, std::move(command.completion));
  webkit_cookie_manager_get_cookies(manager, command.url.c_str(), nullptr, CookiesReady, operation);
}

void ApplyProperties(const std::shared_ptr<LinuxWebViewInstance>& instance, const WebViewProperties& properties,
                     bool initial) {
  const bool url_changed = initial || instance->properties.url != properties.url;
  instance->properties = properties;
  webkit_settings_set_enable_javascript(webkit_web_view_get_settings(instance->web_view),
                                        properties.java_script_enabled);
  if (url_changed) {
    LoadUrl(instance, properties.url);
  }
}

bool RunCommand(const std::weak_ptr<LinuxWebViewInstance>& weak_instance, WebViewControllerCommand command) {
  const std::shared_ptr<LinuxWebViewInstance> instance = weak_instance.lock();
  if (!instance || instance->disposed || instance->web_view == nullptr) {
    return false;
  }
  if (const auto* value = std::get_if<WebViewCommand>(&command)) {
    switch (*value) {
    case WebViewCommand::Reload:
      webkit_web_view_reload(instance->web_view);
      return true;
    case WebViewCommand::StopLoading:
      webkit_web_view_stop_loading(instance->web_view);
      return true;
    case WebViewCommand::GoBack:
      if (!webkit_web_view_can_go_back(instance->web_view)) return false;
      webkit_web_view_go_back(instance->web_view);
      return true;
    case WebViewCommand::GoForward:
      if (!webkit_web_view_can_go_forward(instance->web_view)) return false;
      webkit_web_view_go_forward(instance->web_view);
      return true;
    }
  }
  if (const auto* value = std::get_if<WebViewLoadRequestCommand>(&command)) {
    LoadRequest(instance, value->request);
    return true;
  }
  if (const auto* value = std::get_if<WebViewLoadHtmlCommand>(&command)) {
    LoadHtml(instance, value->html);
    return true;
  }
  if (auto* value = std::get_if<WebViewCookieCommand>(&command)) {
    RunCookieCommand(instance, std::move(*value));
    return true;
  }
  EvaluateJavaScript(instance, std::get<WebViewEvaluateJavaScriptCommand>(std::move(command)));
  return true;
}

void MessageReceived(WebKitUserContentManager*, JSCValue* value, gpointer data) {
  auto* instance = static_cast<LinuxWebViewInstance*>(data);
  if (instance == nullptr || instance->disposed || !jsc_value_is_string(value)) {
    return;
  }
  gchar* message = jsc_value_to_string(value);
  if (message != nullptr) {
    instance->events.Emit<WebViewEvents::MessageReceived>(std::string(message));
    g_free(message);
  }
}

gboolean DecidePolicy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer data) {
  auto* raw = static_cast<LinuxWebViewInstance*>(data);
  if (raw == nullptr || raw->disposed || type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
    return FALSE;
  }
  const std::shared_ptr<LinuxWebViewInstance> instance = raw->shared_from_this();
  auto* navigation = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
  WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(navigation);
  WebKitURIRequest* request = action != nullptr ? webkit_navigation_action_get_request(action) : nullptr;
  const char* uri = request != nullptr ? webkit_uri_request_get_uri(request) : nullptr;
  const char* target_frame = action != nullptr ? webkit_navigation_action_get_frame_name(action) : nullptr;
  const bool main_frame = target_frame == nullptr || *target_frame == '\0';
  const std::optional<bool> allowed = instance->events.Emit<WebViewEvents::NavigationRequested>(
      WebViewNavigationRequest{.url = uri != nullptr ? uri : "", .is_main_frame = main_frame});
  if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION || (allowed.has_value() && !*allowed)) {
    webkit_policy_decision_ignore(decision);
  } else {
    webkit_policy_decision_use(decision);
  }
  return TRUE;
}

void LoadChanged(WebKitWebView*, WebKitLoadEvent event, gpointer data) {
  auto* raw = static_cast<LinuxWebViewInstance*>(data);
  if (raw == nullptr || raw->disposed) {
    return;
  }
  const std::shared_ptr<LinuxWebViewInstance> instance = raw->shared_from_this();
  switch (event) {
  case WEBKIT_LOAD_STARTED:
    instance->loading = true;
    instance->load_failed = false;
    EmitNavigationState<WebViewEvents::LoadStarted>(instance);
    break;
  case WEBKIT_LOAD_REDIRECTED:
  case WEBKIT_LOAD_COMMITTED:
    break;
  case WEBKIT_LOAD_FINISHED:
    instance->loading = false;
    if (!instance->load_failed) {
      EmitNavigationState<WebViewEvents::LoadFinished>(instance);
    }
    break;
  }
  EmitNavigationState<WebViewEvents::NavigationChanged>(instance);
}

gboolean LoadFailed(WebKitWebView*, WebKitLoadEvent, const gchar* uri, GError* error, gpointer data) {
  auto* raw = static_cast<LinuxWebViewInstance*>(data);
  if (raw == nullptr || raw->disposed) {
    return FALSE;
  }
  raw->load_failed = true;
  EmitLoadError(raw->shared_from_this(), "linux/webview/load", error != nullptr && error->message != nullptr
                    ? error->message : "WebKitGTK could not load the page",
                uri != nullptr ? std::optional<std::string>{uri} : std::nullopt);
  return FALSE;
}

void NavigationPropertyChanged(GObject*, GParamSpec*, gpointer data) {
  auto* raw = static_cast<LinuxWebViewInstance*>(data);
  if (raw != nullptr && !raw->disposed) {
    EmitNavigationState<WebViewEvents::NavigationChanged>(raw->shared_from_this());
  }
}

std::shared_ptr<LinuxWebViewInstance> CreateLinuxWebView(GtkWidget* parent, const WebViewProperties& properties,
                                                         PlatformEventEmitter events) {
  if (parent == nullptr || !GTK_IS_FIXED(parent)) {
    throw std::invalid_argument("HuxerUI WebView requires a GTK4 clipping container");
  }
  auto instance = std::make_shared<LinuxWebViewInstance>();
  instance->events = std::move(events);
  GtkWidget* widget = webkit_web_view_new();
  if (widget == nullptr) {
    throw std::runtime_error("WebKitGTK could not create a WebView widget");
  }
  instance->web_view = WEBKIT_WEB_VIEW(widget);
  gtk_widget_set_focusable(widget, TRUE);
  gtk_widget_set_hexpand(widget, TRUE);
  gtk_widget_set_vexpand(widget, TRUE);
  gtk_fixed_put(GTK_FIXED(parent), widget, 0.0, 0.0);

  WebKitUserContentManager* manager = webkit_web_view_get_user_content_manager(instance->web_view);
  g_signal_connect(manager, "script-message-received::huxerui", G_CALLBACK(MessageReceived), instance.get());
  if (!webkit_user_content_manager_register_script_message_handler(manager, "huxerui", nullptr)) {
    throw std::runtime_error("WebKitGTK could not register the HuxerUI script message handler");
  }
  WebKitUserScript* bridge_script = webkit_user_script_new(
      "Object.defineProperty(window, 'huxerui', { value: Object.freeze({ postMessage: function(value) { "
      "window.webkit.messageHandlers.huxerui.postMessage(String(value)); } }), configurable: false });",
      WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(manager, bridge_script);
  webkit_user_script_unref(bridge_script);

  g_signal_connect(instance->web_view, "decide-policy", G_CALLBACK(DecidePolicy), instance.get());
  g_signal_connect(instance->web_view, "load-changed", G_CALLBACK(LoadChanged), instance.get());
  g_signal_connect(instance->web_view, "load-failed", G_CALLBACK(LoadFailed), instance.get());
  g_signal_connect(instance->web_view, "notify::estimated-load-progress", G_CALLBACK(NavigationPropertyChanged), instance.get());
  g_signal_connect(instance->web_view, "notify::uri", G_CALLBACK(NavigationPropertyChanged), instance.get());
  g_signal_connect(instance->web_view, "notify::title", G_CALLBACK(NavigationPropertyChanged), instance.get());
  g_signal_connect(instance->web_view, "notify::can-go-back", G_CALLBACK(NavigationPropertyChanged), instance.get());
  g_signal_connect(instance->web_view, "notify::can-go-forward", G_CALLBACK(NavigationPropertyChanged), instance.get());
  ApplyProperties(instance, properties, true);
  return instance;
}

void DisposeLinuxWebView(LinuxWebViewInstance& instance) {
  instance.disposed = true;
  if (instance.web_view == nullptr) {
    return;
  }
  webkit_web_view_stop_loading(instance.web_view);
  WebKitUserContentManager* manager = webkit_web_view_get_user_content_manager(instance.web_view);
  g_signal_handlers_disconnect_by_data(manager, &instance);
  webkit_user_content_manager_unregister_script_message_handler(manager, "huxerui", nullptr);
  g_signal_handlers_disconnect_by_data(instance.web_view, &instance);
  instance.web_view = nullptr;
}

} // namespace

void InstallPlatformWebView(ApplicationContext& root) {
  linux::PlatformViewFactory<WebViewProperties, LinuxWebViewInstance, WebViewController> factory{
      .create = CreateLinuxWebView,
      .view = [](const std::shared_ptr<LinuxWebViewInstance>& instance) -> GtkWidget* {
        return instance->web_view != nullptr ? GTK_WIDGET(instance->web_view) : nullptr;
      },
      .update = [](LinuxWebViewInstance& value, const WebViewProperties& properties) {
        ApplyProperties(value.shared_from_this(), properties, false);
      },
      .dispose = DisposeLinuxWebView,
      .connect = [](LinuxWebViewInstance& value, const WebViewController& controller) {
        std::weak_ptr<LinuxWebViewInstance> weak_instance = value.shared_from_this();
        WebViewControllerAccess::Connect(controller, [weak_instance](WebViewControllerCommand command) {
          return RunCommand(weak_instance, std::move(command));
        });
      },
      .disconnect = [](LinuxWebViewInstance&, const WebViewController& controller) {
        WebViewControllerAccess::Disconnect(controller);
      },
  };
  root.RegisterPlatformView<WebViewProperties, WebViewController>(std::string(web_view_type), std::move(factory));
}

} // namespace huxerui::detail
