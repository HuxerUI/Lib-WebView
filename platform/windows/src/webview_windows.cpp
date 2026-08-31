#include <windows.h>
#include <wrl.h>
#include <shlwapi.h>

#include <WebView2.h>

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

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
  const std::wstring native_method = Utf8ToWide(method);
  std::string header_text;
  for (const auto& [name, value] : request.headers) {
    header_text += name + ": " + value + "\r\n";
  }
  const std::wstring headers = Utf8ToWide(header_text);
  if (url.empty() || native_method.empty() || (!header_text.empty() && headers.empty())) {
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

  ComPtr<ICoreWebView2WebResourceRequest> native_request;
  const HRESULT created = environment->CreateWebResourceRequest(
      url.c_str(),
      native_method.c_str(),
      body.Get(),
      headers.empty() ? nullptr : headers.c_str(),
      &native_request
  );
  if (FAILED(created) || !native_request) {
    EmitLoadError(instance, "webview/invalid-request", "Could not create the WebView request");
    return true;
  }
  if (FAILED(web_view->NavigateWithWebResourceRequest(native_request.Get()))) {
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
  if (!instance || instance->disposed || !instance->web_view) {
    return false;
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
  if (const auto* value = std::get_if<WebViewLoadRequestCommand>(&command)) {
    return LoadRequest(instance, value->request);
  }
  if (const auto* value = std::get_if<WebViewLoadHtmlCommand>(&command)) {
    return LoadHtml(instance, value->html);
  }
  return EvaluateJavaScript(
      instance,
      std::get<WebViewEvaluateJavaScriptCommand>(std::move(command))
  );
}

void DisposeWindowsWebView(WindowsWebViewInstance& instance) {
  instance.disposed = true;
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

void InstallPlatformWebView(RootContext& root) {
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
#include <algorithm>
#include <cctype>
#include <limits>
