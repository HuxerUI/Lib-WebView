#include <windows.h>
#include <wrl.h>
#include <shlwapi.h>

#include <WebView2.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/webview.h>
#include <huxerui/windows/platform_registry.h>

#include "detail/webview_internal.h"

namespace huxerui::detail {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t web_view_window_class[] = L"HuxerUI.WebView.Host";

std::string WideToUtf8(const wchar_t* value) {
  if (value == nullptr || *value == L'\0') {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
  result.pop_back();
  return result;
}

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                      result.data(), size);
  return result;
}

struct WindowsWebViewInstance : std::enable_shared_from_this<WindowsWebViewInstance> {
  HWND window = nullptr;
  WebViewProperties properties;
  PlatformEventEmitter events;
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> web_view;
  EventRegistrationToken navigation_starting_token{};
  EventRegistrationToken navigation_completed_token{};
  EventRegistrationToken history_changed_token{};
  EventRegistrationToken title_changed_token{};
  EventRegistrationToken web_message_received_token{};
  // Commands can arrive between the platform view connecting and WebView2 finishing
  // initialization; they are queued instead of being dropped, then drained in order.
  std::vector<WebViewControllerCommand> pending_commands;
  bool events_attached = false;
  bool loading = false;
  bool disposed = false;

  void UpdateBounds() const {
    if (!controller || window == nullptr) {
      return;
    }
    RECT bounds{};
    GetClientRect(window, &bounds);
    controller->put_Bounds(bounds);
  }

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM word, LPARAM data) {
    WindowsWebViewInstance* instance =
        reinterpret_cast<WindowsWebViewInstance*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(data);
      instance = static_cast<WindowsWebViewInstance*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
    }
    if (instance != nullptr) {
      if (message == WM_SIZE) {
        instance->UpdateBounds();
      } else if (message == WM_SETFOCUS && instance->controller) {
        instance->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      } else if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      }
    }
    return DefWindowProcW(window, message, word, data);
  }
};

void EnsureWindowClass() {
  static std::once_flag once;
  std::call_once(once, [] {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowsWebViewInstance::WindowProcedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = web_view_window_class;
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      throw std::runtime_error("Could not register the WebView host window class");
    }
  });
}

std::string CurrentUrl(const WindowsWebViewInstance& instance) {
  if (!instance.web_view) {
    return instance.properties.url;
  }
  wchar_t* value = nullptr;
  if (FAILED(instance.web_view->get_Source(&value))) {
    return instance.properties.url;
  }
  const std::string result = WideToUtf8(value);
  CoTaskMemFree(value);
  return result;
}

WebViewNavigationState NavigationState(const std::shared_ptr<WindowsWebViewInstance>& instance) {
  wchar_t* title_value = nullptr;
  BOOL can_go_back = FALSE;
  BOOL can_go_forward = FALSE;
  instance->web_view->get_DocumentTitle(&title_value);
  instance->web_view->get_CanGoBack(&can_go_back);
  instance->web_view->get_CanGoForward(&can_go_forward);
  const std::string title = WideToUtf8(title_value);
  CoTaskMemFree(title_value);
  return {
      .url = CurrentUrl(*instance),
      .title = title,
      .is_loading = instance->loading,
      .progress = instance->loading ? 0.0 : 1.0,
      .can_go_back = can_go_back != FALSE,
      .can_go_forward = can_go_forward != FALSE,
  };
}

template <class Event>
void EmitStateEvent(const std::shared_ptr<WindowsWebViewInstance>& instance) {
  if (!instance || instance->disposed || !instance->web_view) {
    return;
  }
  instance->events.Emit<Event>(NavigationState(instance));
}

void EmitNavigationState(const std::shared_ptr<WindowsWebViewInstance>& instance) {
  if (!instance || instance->disposed || !instance->web_view) {
    return;
  }
  instance->events.Emit<WebViewEvents::NavigationChanged>(NavigationState(instance));
}

std::string UppercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return value;
}

std::string EscapeHtmlAttribute(const std::string& value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '&':
        result += "&amp;";
        break;
      case '"':
        result += "&quot;";
        break;
      case '<':
        result += "&lt;";
        break;
      case '>':
        result += "&gt;";
        break;
      default:
        result += character;
        break;
    }
  }
  return result;
}

std::string HtmlWithBaseUrl(const WebViewHtml& html) {
  if (html.base_url.empty()) {
    return html.content;
  }
  return "<head><base href=\"" + EscapeHtmlAttribute(html.base_url) + "\"></head>" + html.content;
}

void EmitLoadError(
    const std::shared_ptr<WindowsWebViewInstance>& instance,
    std::string code,
    std::string message
);

bool LoadRequest(
    const std::shared_ptr<WindowsWebViewInstance>& instance,
    const WebViewRequest& request
) {
  const std::string method = UppercaseAscii(request.method);
  if (method != "GET" && method != "POST") {
    EmitLoadError(
        instance,
        "webview/unsupported-request",
        "WebView2 navigation supports only GET and POST requests"
    );
    return true;
  }

  ComPtr<ICoreWebView2Environment2> environment;
  ComPtr<ICoreWebView2_2> web_view;
  if (!instance->environment ||
      FAILED(instance->environment.As(&environment)) ||
      FAILED(instance->web_view.As(&web_view))) {
    EmitLoadError(
        instance,
        "webview/unsupported-request",
        "The installed WebView2 Runtime does not support custom navigation requests"
    );
    return true;
  }

  const std::wstring url = Utf8ToWide(request.url);
  const std::wstring wide_method = Utf8ToWide(method);
  std::string header_text;
  for (const auto& [name, value] : request.headers) {
    header_text += name + ": " + value + "\r\n";
  }
  const std::wstring headers = Utf8ToWide(header_text);
  if (url.empty() || wide_method.empty() || (!header_text.empty() && headers.empty())) {
    EmitLoadError(instance, "webview/invalid-request", "The WebView request is not valid UTF-8");
    return true;
  }

  ComPtr<IStream> body;
  if (request.body.has_value()) {
    if (request.body->size() > std::numeric_limits<UINT>::max()) {
      EmitLoadError(instance, "webview/request-too-large", "The WebView request body is too large");
      return true;
    }
    body.Attach(SHCreateMemStream(
        reinterpret_cast<const BYTE*>(request.body->data()),
        static_cast<UINT>(request.body->size())
    ));
    if (!body) {
      EmitLoadError(instance, "webview/request-body", "Could not create the WebView request body");
      return true;
    }
  }

  ComPtr<ICoreWebView2WebResourceRequest> platform_request;
  const HRESULT created = environment->CreateWebResourceRequest(
      url.c_str(),
      wide_method.c_str(),
      body.Get(),
      headers.empty() ? nullptr : headers.c_str(),
      &platform_request
  );
  if (FAILED(created) || !platform_request) {
    EmitLoadError(instance, "webview/invalid-request", "Could not create the WebView request");
    return true;
  }
  if (FAILED(web_view->NavigateWithWebResourceRequest(platform_request.Get()))) {
    EmitLoadError(instance, "webview/request-failed", "Could not load the WebView request");
  }
  return true;
}

bool LoadHtml(const std::shared_ptr<WindowsWebViewInstance>& instance, const WebViewHtml& html) {
  const std::wstring content = Utf8ToWide(HtmlWithBaseUrl(html));
  if (content.empty() && !html.content.empty()) {
    EmitLoadError(instance, "webview/invalid-html", "The WebView HTML is not valid UTF-8");
    return true;
  }
  if (FAILED(instance->web_view->NavigateToString(content.c_str()))) {
    EmitLoadError(instance, "webview/html-failed", "Could not load the WebView HTML");
  }
  return true;
}

bool GetCookiesCommand(const std::shared_ptr<WindowsWebViewInstance>& instance, WebViewGetCookiesCommand command) {
  ComPtr<ICoreWebView2_2> view;
  ComPtr<ICoreWebView2CookieManager> manager;
  if (FAILED(instance->web_view.As(&view)) || FAILED(view->get_CookieManager(&manager))) {
    command.completion(PlatformError{"webview/unsupported-cookies", "Cookie manager is unavailable", {}});
    return true;
  }
  const std::wstring url = Utf8ToWide(command.url);
  if (url.empty()) {
    command.completion(PlatformError{"webview/invalid-url", "Cookie URL is not valid UTF-8", {}});
    return true;
  }
  const std::weak_ptr<WindowsWebViewInstance> weak = instance;
  auto completion = std::move(command.completion);
  const HRESULT started = manager->GetCookies(url.c_str(),
      Callback<ICoreWebView2GetCookiesCompletedHandler>(
          [weak, completion](HRESULT status, ICoreWebView2CookieList* list) -> HRESULT {
            auto live = weak.lock();
            if (!live || live->disposed) return S_OK;
            UINT count = 0;
            if (FAILED(status) || !list || FAILED(list->get_Count(&count))) {
              completion(PlatformError{"webview/cookies", "Could not read cookies", {}});
              return S_OK;
            }
            std::string header;
            for (UINT i = 0; i < count; ++i) {
              ComPtr<ICoreWebView2Cookie> cookie;
              if (FAILED(list->GetValueAtIndex(i, &cookie))) {
                completion(PlatformError{"webview/cookies", "Could not read cookie entry", {}});
                return S_OK;
              }
              LPWSTR name = nullptr, value = nullptr;
              const HRESULT name_status = cookie->get_Name(&name);
              const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_name(name, &CoTaskMemFree);
              const HRESULT value_status = cookie->get_Value(&value);
              const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_value(value, &CoTaskMemFree);
              const std::string key = WideToUtf8(owned_name.get()), text = WideToUtf8(owned_value.get());
              if (FAILED(name_status) || FAILED(value_status)) {
                completion(PlatformError{"webview/cookies", "Could not read cookie value", {}});
                return S_OK;
              }
              if (!header.empty()) header += "; ";
              header += key + "=" + text;
            }
            completion(std::move(header));
            return S_OK;
          }).Get());
  if (FAILED(started)) completion(PlatformError{"webview/cookies", "Could not start cookie query", {}});
  return true;
}

bool ClearCookiesCommand(const std::shared_ptr<WindowsWebViewInstance>& instance, WebViewClearCookiesCommand command) {
  ComPtr<ICoreWebView2_2> view;
  ComPtr<ICoreWebView2CookieManager> manager;
  if (FAILED(instance->web_view.As(&view)) || FAILED(view->get_CookieManager(&manager))) {
    command.completion(PlatformError{"webview/unsupported-cookies", "Cookie manager is unavailable", {}});
    return true;
  }
  const HRESULT cleared = manager->DeleteAllCookies();
  if (FAILED(cleared)) command.completion(PlatformError{"webview/cookies", "Could not clear cookies", {}});
  else command.completion(std::string{});
  return true;
}

bool ReadCookie(ICoreWebView2Cookie* cookie, WebViewCookie& result) {
  LPWSTR name = nullptr, value = nullptr, domain = nullptr, path = nullptr;
  const HRESULT name_status = cookie->get_Name(&name);
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_name(name, &CoTaskMemFree);
  const HRESULT value_status = cookie->get_Value(&value);
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_value(value, &CoTaskMemFree);
  const HRESULT domain_status = cookie->get_Domain(&domain);
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_domain(domain, &CoTaskMemFree);
  const HRESULT path_status = cookie->get_Path(&path);
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_path(path, &CoTaskMemFree);
  double expires = 0.0;
  BOOL session = TRUE, secure = FALSE, http_only = FALSE;
  COREWEBVIEW2_COOKIE_SAME_SITE_KIND same_site = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE;
  if (FAILED(name_status) || FAILED(value_status) || FAILED(domain_status) || FAILED(path_status) ||
      FAILED(cookie->get_Expires(&expires)) || FAILED(cookie->get_IsSession(&session)) ||
      FAILED(cookie->get_IsSecure(&secure)) || FAILED(cookie->get_IsHttpOnly(&http_only)) ||
      FAILED(cookie->get_SameSite(&same_site))) {
    return false;
  }
  result = WebViewCookie{
      .name = WideToUtf8(owned_name.get()),
      .value = WideToUtf8(owned_value.get()),
      .domain = WideToUtf8(owned_domain.get()),
      .path = WideToUtf8(owned_path.get()),
      .secure = secure != FALSE,
      .http_only = http_only != FALSE,
  };
  if (!session) {
    result.expires = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(expires)));
  }
  if (same_site == COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX) result.same_site = WebViewCookieSameSite::Lax;
  if (same_site == COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT) result.same_site = WebViewCookieSameSite::Strict;
  if (same_site == COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE) result.same_site = WebViewCookieSameSite::None;
  return true;
}

bool SetCookieCommand(const std::shared_ptr<WindowsWebViewInstance>& instance, WebViewSetCookieCommand command) {
  ComPtr<ICoreWebView2_2> view;
  ComPtr<ICoreWebView2CookieManager> manager;
  if (FAILED(instance->web_view.As(&view)) || FAILED(view->get_CookieManager(&manager))) {
    command.completion(PlatformError{"webview/unsupported-cookies", "Cookie manager is unavailable", {}});
    return true;
  }
  if (Utf8ToWide(command.url).empty()) {
    command.completion(PlatformError{"webview/invalid-url", "Cookie URL is not valid UTF-8", {}});
    return true;
  }
  const std::wstring name = Utf8ToWide(command.cookie.name);
  const std::wstring value = Utf8ToWide(command.cookie.value);
  const std::wstring domain = Utf8ToWide(command.cookie.domain);
  const std::wstring path = Utf8ToWide(command.cookie.path);
  if (name.empty() || domain.empty() || path.empty() || (value.empty() && !command.cookie.value.empty())) {
    command.completion(PlatformError{"webview/invalid-cookie", "Cookie text is not valid UTF-8", {}});
    return true;
  }
  ComPtr<ICoreWebView2Cookie> cookie;
  HRESULT status = manager->CreateCookie(name.c_str(), value.c_str(), domain.c_str(), path.c_str(), &cookie);
  if (SUCCEEDED(status) && command.cookie.expires.has_value()) {
    status = cookie->put_Expires(std::chrono::duration<double>(command.cookie.expires->time_since_epoch()).count());
  }
  if (SUCCEEDED(status)) status = cookie->put_IsSecure(command.cookie.secure ? TRUE : FALSE);
  if (SUCCEEDED(status)) status = cookie->put_IsHttpOnly(command.cookie.http_only ? TRUE : FALSE);
  COREWEBVIEW2_COOKIE_SAME_SITE_KIND same_site = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE;
  switch (command.cookie.same_site) {
    case WebViewCookieSameSite::Unspecified:
      break;
    case WebViewCookieSameSite::Lax:
      same_site = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX;
      break;
    case WebViewCookieSameSite::Strict:
      same_site = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT;
      break;
    case WebViewCookieSameSite::None:
      same_site = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE;
      break;
  }
  if (SUCCEEDED(status) && command.cookie.same_site != WebViewCookieSameSite::Unspecified) {
    status = cookie->put_SameSite(same_site);
  }
  if (FAILED(status)) {
    command.completion(PlatformError{"webview/invalid-cookie", "Could not create the WebView2 cookie", {}});
    return true;
  }
  status = manager->AddOrUpdateCookie(cookie.Get());
  if (FAILED(status)) command.completion(PlatformError{"webview/cookies", "Could not set cookie", {}});
  else command.completion(std::string{});
  return true;
}

bool DeleteCookieCommand(const std::shared_ptr<WindowsWebViewInstance>& instance, WebViewDeleteCookieCommand command) {
  ComPtr<ICoreWebView2_2> view;
  ComPtr<ICoreWebView2CookieManager> manager;
  if (FAILED(instance->web_view.As(&view)) || FAILED(view->get_CookieManager(&manager))) {
    command.completion(PlatformError{"webview/unsupported-cookies", "Cookie manager is unavailable", {}});
    return true;
  }
  if (Utf8ToWide(command.url).empty()) {
    command.completion(PlatformError{"webview/invalid-url", "Cookie URL is not valid UTF-8", {}});
    return true;
  }
  const std::wstring name = Utf8ToWide(command.name);
  const std::wstring domain = Utf8ToWide(command.domain);
  const std::wstring path = Utf8ToWide(command.path);
  if (name.empty() || domain.empty() || path.empty()) {
    command.completion(PlatformError{"webview/invalid-cookie", "Cookie identity is not valid UTF-8", {}});
    return true;
  }
  const HRESULT status =
      manager->DeleteCookiesWithDomainAndPath(name.c_str(), domain.c_str(), path.c_str());
  if (FAILED(status)) command.completion(PlatformError{"webview/cookies", "Could not delete cookie", {}});
  else command.completion(std::string{});
  return true;
}

bool GetCookieListCommand(const std::shared_ptr<WindowsWebViewInstance>& instance, WebViewGetCookieListCommand command) {
  ComPtr<ICoreWebView2_2> view;
  ComPtr<ICoreWebView2CookieManager> manager;
  if (FAILED(instance->web_view.As(&view)) || FAILED(view->get_CookieManager(&manager))) {
    command.completion(PlatformError{"webview/unsupported-cookies", "Cookie manager is unavailable", {}});
    return true;
  }
  const std::wstring url = Utf8ToWide(command.url);
  if (url.empty()) {
    command.completion(PlatformError{"webview/invalid-url", "Cookie URL is not valid UTF-8", {}});
    return true;
  }
  const std::weak_ptr<WindowsWebViewInstance> weak = instance;
  auto completion = std::move(command.completion);
  const HRESULT started = manager->GetCookies(url.c_str(),
      Callback<ICoreWebView2GetCookiesCompletedHandler>(
          [weak, completion](HRESULT status, ICoreWebView2CookieList* list) -> HRESULT {
            auto live = weak.lock();
            if (!live || live->disposed) return S_OK;
            UINT count = 0;
            if (FAILED(status) || !list || FAILED(list->get_Count(&count))) {
              completion(PlatformError{"webview/cookies", "Could not read cookies", {}});
              return S_OK;
            }
            std::vector<WebViewCookie> cookies;
            cookies.reserve(count);
            for (UINT i = 0; i < count; ++i) {
              ComPtr<ICoreWebView2Cookie> cookie;
              WebViewCookie value;
              if (FAILED(list->GetValueAtIndex(i, &cookie)) || !ReadCookie(cookie.Get(), value)) {
                completion(PlatformError{"webview/cookies", "Could not read cookie entry", {}});
                return S_OK;
              }
              cookies.push_back(std::move(value));
            }
            completion(std::move(cookies));
            return S_OK;
          }).Get());
  if (FAILED(started)) completion(PlatformError{"webview/cookies", "Could not start cookie query", {}});
  return true;
}

bool EvaluateJavaScript(
    const std::shared_ptr<WindowsWebViewInstance>& instance,
    WebViewEvaluateJavaScriptCommand command
) {
  const std::wstring script = Utf8ToWide(command.script);
  if (script.empty() && !command.script.empty()) {
    command.completion(PlatformError{
        "webview/invalid-javascript",
        "The JavaScript source is not valid UTF-8",
        {},
    });
    return true;
  }
  WebViewJavaScriptCompletion completion = std::move(command.completion);
  const HRESULT started = instance->web_view->ExecuteScript(
      script.c_str(),
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [completion](HRESULT result, LPCWSTR value) mutable -> HRESULT {
            if (FAILED(result)) {
              completion(PlatformError{
                  "webview/javascript",
                  "The WebView could not evaluate JavaScript",
                  {},
              });
            } else {
              completion(WideToUtf8(value));
            }
            return S_OK;
          }
      ).Get()
  );
  if (FAILED(started)) {
    completion(PlatformError{
        "webview/javascript",
        "The WebView could not start JavaScript evaluation",
        {},
    });
  }
  return true;
}

void EmitLoadError(
    const std::shared_ptr<WindowsWebViewInstance>& instance,
    std::string code,
    std::string message
) {
  if (!instance || instance->disposed) {
    return;
  }
  instance->events.Emit<WebViewEvents::LoadFailed>(WebViewLoadError{
      .code = std::move(code),
      .message = std::move(message),
      .url = CurrentUrl(*instance),
  });
}

void ApplyProperties(const std::shared_ptr<WindowsWebViewInstance>& instance, const WebViewProperties& properties) {
  const bool url_changed = instance->properties.url != properties.url;
  instance->properties = properties;
  if (!instance->web_view) {
    return;
  }

  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(instance->web_view->get_Settings(&settings))) {
    settings->put_IsScriptEnabled(properties.java_script_enabled);
    settings->put_IsWebMessageEnabled(TRUE);
  }
  if (url_changed) {
    const std::wstring url = Utf8ToWide(properties.url);
    if (url.empty() && !properties.url.empty()) {
      EmitLoadError(instance, "webview/invalid-url", "The WebView URL is not valid UTF-8");
    } else {
      instance->web_view->Navigate(url.c_str());
    }
  }
}

void AttachEvents(const std::shared_ptr<WindowsWebViewInstance>& instance) {
  std::weak_ptr<WindowsWebViewInstance> weak_instance = instance;

  instance->web_view->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [weak_instance](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* arguments) -> HRESULT {
            std::shared_ptr<WindowsWebViewInstance> current = weak_instance.lock();
            if (!current || current->disposed) {
              return S_OK;
            }
            wchar_t* url_value = nullptr;
            arguments->get_Uri(&url_value);
            const std::string url = WideToUtf8(url_value);
            CoTaskMemFree(url_value);
            const std::optional<bool> allowed =
                current->events.Emit<WebViewEvents::NavigationRequested>(WebViewNavigationRequest{
                    .url = url,
                    .is_main_frame = true,
                });
            if (allowed.has_value() && !*allowed) {
              arguments->put_Cancel(TRUE);
              return S_OK;
            }
            current->properties.url = url;
            current->loading = true;
            EmitStateEvent<WebViewEvents::LoadStarted>(current);
            EmitNavigationState(current);
            return S_OK;
          })
          .Get(),
      &instance->navigation_starting_token
  );

  instance->web_view->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [weak_instance](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* arguments) -> HRESULT {
            std::shared_ptr<WindowsWebViewInstance> current = weak_instance.lock();
            if (!current || current->disposed) {
              return S_OK;
            }
            current->loading = false;
            BOOL successful = FALSE;
            arguments->get_IsSuccess(&successful);
            if (successful == FALSE) {
              COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
              arguments->get_WebErrorStatus(&status);
              EmitLoadError(
                  current,
                  "windows/webview2/" + std::to_string(static_cast<int>(status)),
                  "The WebView navigation failed"
              );
            }
            if (successful != FALSE) {
              EmitStateEvent<WebViewEvents::LoadFinished>(current);
            }
            EmitNavigationState(current);
            return S_OK;
          })
          .Get(),
      &instance->navigation_completed_token
  );

  instance->web_view->add_HistoryChanged(
      Callback<ICoreWebView2HistoryChangedEventHandler>(
          [weak_instance](ICoreWebView2*, IUnknown*) -> HRESULT {
            EmitNavigationState(weak_instance.lock());
            return S_OK;
          })
          .Get(),
      &instance->history_changed_token
  );

  instance->web_view->add_DocumentTitleChanged(
      Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
          [weak_instance](ICoreWebView2*, IUnknown*) -> HRESULT {
            EmitNavigationState(weak_instance.lock());
            return S_OK;
          })
          .Get(),
      &instance->title_changed_token
  );

  instance->web_view->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [weak_instance](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* arguments) -> HRESULT {
            std::shared_ptr<WindowsWebViewInstance> current = weak_instance.lock();
            if (!current || current->disposed) {
              return S_OK;
            }
            wchar_t* message = nullptr;
            if (SUCCEEDED(arguments->TryGetWebMessageAsString(&message))) {
              current->events.Emit<WebViewEvents::MessageReceived>(WideToUtf8(message));
            }
            CoTaskMemFree(message);
            return S_OK;
          }
      ).Get(),
      &instance->web_message_received_token
  );

  instance->web_view->AddScriptToExecuteOnDocumentCreated(
      L"Object.defineProperty(window, 'huxerui', { value: Object.freeze({ postMessage: function(value) { window.chrome.webview.postMessage(String(value)); } }), configurable: false });",
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          [](HRESULT, LPCWSTR) -> HRESULT {
            return S_OK;
          }
      ).Get()
  );
  instance->events_attached = true;
}

void DrainPendingCommands(const std::weak_ptr<WindowsWebViewInstance>& instance);

void InitializeWebView(const std::shared_ptr<WindowsWebViewInstance>& instance) {
  std::weak_ptr<WindowsWebViewInstance> weak_instance = instance;
  const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
      nullptr,
      nullptr,
      nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [weak_instance](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            std::shared_ptr<WindowsWebViewInstance> current = weak_instance.lock();
            if (!current || current->disposed) {
              return S_OK;
            }
            if (FAILED(result) || environment == nullptr) {
              current->pending_commands.clear();
              EmitLoadError(
                  current,
                  "windows/webview2/environment",
                  "Could not create the WebView2 environment"
              );
              return S_OK;
            }
            current->environment = environment;
            return environment->CreateCoreWebView2Controller(
                current->window,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [weak_instance](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT {
                      std::shared_ptr<WindowsWebViewInstance> value = weak_instance.lock();
                      if (!value || value->disposed) {
                        if (controller != nullptr) {
                          controller->Close();
                        }
                        return S_OK;
                      }
                      if (FAILED(controller_result) || controller == nullptr) {
                        value->pending_commands.clear();
                        EmitLoadError(
                            value,
                            "windows/webview2/controller",
                            "Could not create the WebView2 controller"
                        );
                        return S_OK;
                      }
                      value->controller = controller;
                      controller->get_CoreWebView2(&value->web_view);
                      if (!value->web_view) {
                        value->pending_commands.clear();
                        EmitLoadError(
                            value,
                            "windows/webview2/view",
                            "Could not create the WebView2 view"
                        );
                        return S_OK;
                      }
                      value->UpdateBounds();
                      AttachEvents(value);
                      const WebViewProperties properties = value->properties;
                      value->properties.url.clear();
                      ApplyProperties(value, properties);
                      DrainPendingCommands(weak_instance);
                      return S_OK;
                    })
                    .Get()
            );
          })
          .Get()
  );
  if (FAILED(started)) {
    EmitLoadError(instance, "windows/webview2/runtime", "Could not start the WebView2 Runtime");
  }
}

std::shared_ptr<WindowsWebViewInstance> CreateWindowsWebView(
    HWND parent,
    const WebViewProperties& properties,
    PlatformEventEmitter events
) {
  EnsureWindowClass();
  auto instance = std::make_shared<WindowsWebViewInstance>();
  instance->properties = properties;
  instance->events = std::move(events);
  instance->window = CreateWindowExW(
      0,
      web_view_window_class,
      L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
      0,
      0,
      1,
      1,
      parent,
      nullptr,
      GetModuleHandleW(nullptr),
      instance.get()
  );
  if (instance->window == nullptr) {
    throw std::runtime_error("Could not create the WebView host window");
  }
  InitializeWebView(instance);
  return instance;
}

bool RunCommand(
    const std::weak_ptr<WindowsWebViewInstance>& weak_instance,
    WebViewControllerCommand command
) {
  std::shared_ptr<WindowsWebViewInstance> instance = weak_instance.lock();
  if (!instance || instance->disposed) {
    return false;
  }
  // The controller connects as soon as the platform view is created, while the WebView2
  // environment/controller are still being created asynchronously. Queue instead of dropping.
  if (!instance->web_view) {
    instance->pending_commands.push_back(std::move(command));
    return true;
  }
  if (const auto* value = std::get_if<WebViewCommand>(&command)) {
    switch (*value) {
    case WebViewCommand::Reload:
      return SUCCEEDED(instance->web_view->Reload());
    case WebViewCommand::StopLoading:
      return SUCCEEDED(instance->web_view->Stop());
    case WebViewCommand::GoBack: {
      BOOL available = FALSE;
      instance->web_view->get_CanGoBack(&available);
      return available != FALSE && SUCCEEDED(instance->web_view->GoBack());
    }
    case WebViewCommand::GoForward: {
      BOOL available = FALSE;
      instance->web_view->get_CanGoForward(&available);
      return available != FALSE && SUCCEEDED(instance->web_view->GoForward());
    }
    }
  }
  if (const auto* value = std::get_if<WebViewLoadRequestCommand>(&command)) {
    return LoadRequest(instance, value->request);
  }
  if (const auto* value = std::get_if<WebViewLoadHtmlCommand>(&command)) {
    return LoadHtml(instance, value->html);
  }
  if (auto* value = std::get_if<WebViewGetCookiesCommand>(&command)) {
    return GetCookiesCommand(instance, std::move(*value));
  }
  if (auto* value = std::get_if<WebViewClearCookiesCommand>(&command)) {
    return ClearCookiesCommand(instance, std::move(*value));
  }
  if (auto* value = std::get_if<WebViewSetCookieCommand>(&command)) {
    return SetCookieCommand(instance, std::move(*value));
  }
  if (auto* value = std::get_if<WebViewDeleteCookieCommand>(&command)) {
    return DeleteCookieCommand(instance, std::move(*value));
  }
  if (auto* value = std::get_if<WebViewGetCookieListCommand>(&command)) {
    return GetCookieListCommand(instance, std::move(*value));
  }
  return EvaluateJavaScript(
      instance,
      std::get<WebViewEvaluateJavaScriptCommand>(std::move(command))
  );
}

void DrainPendingCommands(const std::weak_ptr<WindowsWebViewInstance>& weak_instance) {
  std::shared_ptr<WindowsWebViewInstance> instance = weak_instance.lock();
  if (!instance || instance->disposed || !instance->web_view) {
    return;
  }
  std::vector<WebViewControllerCommand> pending = std::move(instance->pending_commands);
  instance->pending_commands.clear();
  for (auto& command : pending) {
    RunCommand(weak_instance, std::move(command));
  }
}

void DisposeWindowsWebView(WindowsWebViewInstance& instance) {
  instance.disposed = true;
  instance.pending_commands.clear();
  if (instance.events_attached && instance.web_view) {
    instance.web_view->remove_NavigationStarting(instance.navigation_starting_token);
    instance.web_view->remove_NavigationCompleted(instance.navigation_completed_token);
    instance.web_view->remove_HistoryChanged(instance.history_changed_token);
    instance.web_view->remove_DocumentTitleChanged(instance.title_changed_token);
    instance.web_view->remove_WebMessageReceived(instance.web_message_received_token);
  }
  instance.events_attached = false;
  instance.web_view.Reset();
  instance.environment.Reset();
  if (instance.controller) {
    instance.controller->Close();
    instance.controller.Reset();
  }
  if (instance.window != nullptr) {
    DestroyWindow(instance.window);
    instance.window = nullptr;
  }
}

} // namespace

void InstallPlatformWebView(ApplicationContext& root) {
  windows::PlatformViewFactory<WebViewProperties, WindowsWebViewInstance, WebViewController> factory{
      .create = CreateWindowsWebView,
      .view = [](const std::shared_ptr<WindowsWebViewInstance>& instance) {
        return instance->window;
      },
      .update = [](WindowsWebViewInstance& value, const WebViewProperties& properties) {
        ApplyProperties(value.shared_from_this(), properties);
      },
      .dispose = DisposeWindowsWebView,
      .connect = [](WindowsWebViewInstance& value, const WebViewController& controller) {
        std::weak_ptr<WindowsWebViewInstance> weak_instance = value.shared_from_this();
        WebViewControllerAccess::Connect(
            controller,
            [weak_instance](WebViewControllerCommand command) {
              return RunCommand(weak_instance, std::move(command));
            }
        );
      },
      .disconnect = [](WindowsWebViewInstance&, const WebViewController& controller) {
        WebViewControllerAccess::Disconnect(controller);
      },
  };

  root.RegisterPlatformView<WebViewProperties, WebViewController>(web_view_type, std::move(factory));
}

} // namespace huxerui::detail
