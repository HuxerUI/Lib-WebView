#include <emscripten.h>
#include <emscripten/val.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <huxerui/web/platform_registry.h>
#include <huxerui/webview.h>

#include "detail/webview_internal.h"

namespace huxerui::detail {

struct WebPlatformViewInstance : std::enable_shared_from_this<WebPlatformViewInstance> {
  std::uint32_t identity = 0;
  emscripten::val element = emscripten::val::undefined();
  WebViewProperties properties;
  std::optional<WebViewHtml> html;
  PlatformEventEmitter events;
  bool loading = false;
  bool disposed = false;
};

namespace {

std::unordered_map<std::uint32_t, std::weak_ptr<WebPlatformViewInstance>>& Instances() {
  static std::unordered_map<std::uint32_t, std::weak_ptr<WebPlatformViewInstance>> instances;
  return instances;
}

std::uint32_t NextIdentity() {
  static std::uint32_t identity = 0;
  do {
    ++identity;
  } while (identity == 0 || Instances().contains(identity));
  return identity;
}

EM_JS(void, BindLoadEvent, (emscripten::EM_VAL element_handle, std::uint32_t identity), {
  const element = Emval.toValue(element_handle);
  element.addEventListener("load", () => {
    try {
      Object.defineProperty(element.contentWindow, "huxerui", {
        value: Object.freeze({
          postMessage(value) {
            const pointer = stringToNewUTF8(String(value));
            Module._huxerui_webview_did_receive_message(identity, pointer);
            _free(pointer);
          },
        }),
        configurable: false,
      });
    } catch (_) {
    }
    Module._huxerui_webview_did_load(identity);
  });
});

EM_JS(char*, EvaluateScript, (emscripten::EM_VAL element_handle, const char* script), {
  const element = Emval.toValue(element_handle);
  try {
    const value = element.contentWindow.eval(UTF8ToString(script));
    const json = JSON.stringify(value);
    return stringToNewUTF8("s" + (json === undefined ? "null" : json));
  } catch (error) {
    return stringToNewUTF8("e" + String(error && error.message ? error.message : error));
  }
});

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

WebViewNavigationState NavigationState(
    const std::shared_ptr<WebPlatformViewInstance>& instance,
    bool loading
) {
  return {
      .url = instance->properties.url,
      .title = {},
      .is_loading = loading,
      .progress = loading ? 0.0 : 1.0,
      .can_go_back = false,
      .can_go_forward = false,
  };
}

template <class Event>
void EmitStateEvent(const std::shared_ptr<WebPlatformViewInstance>& instance, bool loading) {
  if (!instance || instance->disposed) {
    return;
  }
  instance->events.Emit<Event>(NavigationState(instance, loading));
}

void EmitNavigationState(const std::shared_ptr<WebPlatformViewInstance>& instance, bool loading) {
  if (!instance || instance->disposed) {
    return;
  }
  instance->events.Emit<WebViewEvents::NavigationChanged>(WebViewNavigationState{
      .url = instance->properties.url,
      .title = {},
      .is_loading = loading,
      .progress = loading ? 0.0 : 1.0,
      .can_go_back = false,
      .can_go_forward = false,
  });
}

void EmitLoadError(
    const std::shared_ptr<WebPlatformViewInstance>& instance,
    std::string code,
    std::string message,
    std::optional<std::string> url = std::nullopt
) {
  if (!instance || instance->disposed) {
    return;
  }
  instance->events.Emit<WebViewEvents::LoadFailed>(WebViewLoadError{
      .code = std::move(code),
      .message = std::move(message),
      .url = std::move(url),
  });
}

void ApplySandbox(WebPlatformViewInstance& instance) {
  if (instance.properties.java_script_enabled) {
    instance.element.call<void>("removeAttribute", std::string("sandbox"));
  } else {
    instance.element.call<void>("setAttribute", std::string("sandbox"), std::string{});
  }
}

void BeginUrlLoad(
    const std::shared_ptr<WebPlatformViewInstance>& instance,
    const std::string& url
) {
  instance->html.reset();
  instance->loading = true;
  EmitStateEvent<WebViewEvents::LoadStarted>(instance, true);
  EmitNavigationState(instance, true);
  instance->element.call<void>("removeAttribute", std::string("srcdoc"));
  instance->element.set("src", url);
}

void BeginHtmlLoad(
    const std::shared_ptr<WebPlatformViewInstance>& instance,
    const WebViewHtml& html
) {
  instance->html = html;
  instance->loading = true;
  EmitStateEvent<WebViewEvents::LoadStarted>(instance, true);
  EmitNavigationState(instance, true);
  instance->element.call<void>("removeAttribute", std::string("src"));
  instance->element.set("srcdoc", HtmlWithBaseUrl(html));
}

void ApplyProperties(
    const std::shared_ptr<WebPlatformViewInstance>& instance,
    const WebViewProperties& properties,
    bool initial
) {
  const bool url_changed = initial || instance->properties.url != properties.url;
  const bool script_changed = !initial &&
      instance->properties.java_script_enabled != properties.java_script_enabled;

  if (url_changed && !initial) {
    const std::optional<bool> allowed =
        instance->events.Emit<WebViewEvents::NavigationRequested>(WebViewNavigationRequest{
            .url = properties.url,
            .is_main_frame = true,
        });
    if (allowed.has_value() && !*allowed) {
      return;
    }
  }

  instance->properties = properties;
  ApplySandbox(*instance);
  if (initial) {
    const std::uint32_t identity = instance->identity;
    emscripten_async_call(
        [](void* value) {
          const std::uint32_t current_identity = static_cast<std::uint32_t>(
              reinterpret_cast<std::uintptr_t>(value)
          );
          const auto iterator = Instances().find(current_identity);
          if (iterator == Instances().end()) {
            return;
          }
          std::shared_ptr<WebPlatformViewInstance> current = iterator->second.lock();
          if (!current || current->disposed) {
            return;
          }
          const std::optional<bool> allowed =
              current->events.Emit<WebViewEvents::NavigationRequested>(WebViewNavigationRequest{
                  .url = current->properties.url,
                  .is_main_frame = true,
              });
          if (allowed.has_value() && !*allowed) {
            return;
          }
          BeginUrlLoad(current, current->properties.url);
        },
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(identity)),
        0
    );
    return;
  }
  if (url_changed || script_changed) {
    if (!url_changed && instance->html.has_value()) {
      BeginHtmlLoad(instance, *instance->html);
    } else {
      BeginUrlLoad(instance, properties.url);
    }
  }
}

std::shared_ptr<WebPlatformViewInstance> CreateWebPlatformView(
    const WebViewProperties& properties,
    PlatformEventEmitter events
) {
  auto instance = std::make_shared<WebPlatformViewInstance>();
  instance->identity = NextIdentity();
  instance->events = std::move(events);
  instance->element = emscripten::val::global("document").call<emscripten::val>(
      "createElement",
      std::string("iframe")
  );
  instance->element.call<void>("setAttribute", std::string("title"), std::string("Web content"));
  instance->element.call<void>(
      "setAttribute",
      std::string("referrerpolicy"),
      std::string("strict-origin-when-cross-origin")
  );
  emscripten::val style = instance->element["style"];
  style.set("width", std::string("100%"));
  style.set("height", std::string("100%"));
  style.set("border", std::string("0"));
  style.set("display", std::string("block"));

  Instances().emplace(instance->identity, instance);
  BindLoadEvent(instance->element.as_handle(), instance->identity);
  ApplyProperties(instance, properties, true);
  return instance;
}

bool RunCommand(
    const std::weak_ptr<WebPlatformViewInstance>& weak_instance,
    WebViewControllerCommand command
) {
  std::shared_ptr<WebPlatformViewInstance> instance = weak_instance.lock();
  if (!instance || instance->disposed) {
    return false;
  }

  if (const auto* value = std::get_if<WebViewCommand>(&command)) {
    switch (*value) {
    case WebViewCommand::Reload:
      if (instance->html.has_value()) {
        WebViewHtml html = *instance->html;
        BeginHtmlLoad(instance, html);
      } else {
        BeginUrlLoad(instance, instance->properties.url);
      }
      return true;
    case WebViewCommand::StopLoading:
    case WebViewCommand::GoBack:
    case WebViewCommand::GoForward:
      return false;
    }
  }
  if (const auto* value = std::get_if<WebViewLoadRequestCommand>(&command)) {
    const WebViewRequest& request = value->request;
    if (request.method != "GET" || !request.headers.empty() || request.body.has_value()) {
      EmitLoadError(
          instance,
          "webview/unsupported-request",
          "Web iframe requests support only GET without custom headers or a body",
          request.url
      );
      return true;
    }
    const std::optional<bool> allowed =
        instance->events.Emit<WebViewEvents::NavigationRequested>(WebViewNavigationRequest{
            .url = request.url,
            .is_main_frame = true,
        });
    if (allowed.has_value() && !*allowed) {
      return true;
    }
    instance->properties.url = request.url;
    BeginUrlLoad(instance, request.url);
    return true;
  }
  if (const auto* value = std::get_if<WebViewLoadHtmlCommand>(&command)) {
    const std::string navigation_url = value->html.base_url.empty()
        ? std::string("about:srcdoc")
        : value->html.base_url;
    const std::optional<bool> allowed =
        instance->events.Emit<WebViewEvents::NavigationRequested>(WebViewNavigationRequest{
            .url = navigation_url,
            .is_main_frame = true,
        });
    if (allowed.has_value() && !*allowed) {
      return true;
    }
    instance->properties.url = navigation_url;
    BeginHtmlLoad(instance, value->html);
    return true;
  }
  auto value = std::get<WebViewEvaluateJavaScriptCommand>(std::move(command));
  char* encoded = EvaluateScript(instance->element.as_handle(), value.script.c_str());
  if (encoded == nullptr) {
    value.completion(PlatformError{
        "webview/javascript",
        "The WebView could not evaluate JavaScript",
        {},
    });
    return true;
  }
  std::string result(encoded);
  std::free(encoded);
  if (!result.empty() && result.front() == 's') {
    value.completion(result.substr(1));
  } else {
    value.completion(PlatformError{
        "webview/javascript",
        result.empty() ? "The WebView could not evaluate JavaScript" : result.substr(1),
        {},
    });
  }
  return true;
}

void DisposeWebPlatformView(WebPlatformViewInstance& instance) {
  instance.disposed = true;
  Instances().erase(instance.identity);
  instance.element.call<void>("removeAttribute", std::string("src"));
  instance.element.call<void>("removeAttribute", std::string("srcdoc"));
  instance.element = emscripten::val::undefined();
}

void DidLoad(std::uint32_t identity) {
  const auto iterator = Instances().find(identity);
  if (iterator == Instances().end()) {
    return;
  }
  std::shared_ptr<WebPlatformViewInstance> instance = iterator->second.lock();
  if (!instance || instance->disposed || !instance->loading) {
    return;
  }
  instance->loading = false;
  EmitStateEvent<WebViewEvents::LoadFinished>(instance, false);
  EmitNavigationState(instance, false);
}

void DidReceiveMessage(std::uint32_t identity, const char* message) {
  const auto iterator = Instances().find(identity);
  if (iterator == Instances().end()) {
    return;
  }
  std::shared_ptr<WebPlatformViewInstance> instance = iterator->second.lock();
  if (!instance || instance->disposed) {
    return;
  }
  instance->events.Emit<WebViewEvents::MessageReceived>(
      message == nullptr ? std::string{} : std::string(message)
  );
}

} // namespace

void InstallPlatformWebView(RootContext& root) {
  web::PlatformViewFactory<WebViewProperties, WebPlatformViewInstance, WebViewController> factory{
      .create = CreateWebPlatformView,
      .view = [](const std::shared_ptr<WebPlatformViewInstance>& instance) {
        return instance->element;
      },
      .update = [](WebPlatformViewInstance& value, const WebViewProperties& properties) {
        ApplyProperties(value.shared_from_this(), properties, false);
      },
      .dispose = DisposeWebPlatformView,
      .connect = [](WebPlatformViewInstance& value, const WebViewController& controller) {
        std::weak_ptr<WebPlatformViewInstance> weak_instance = value.shared_from_this();
        WebViewControllerAccess::Connect(
            controller,
            [weak_instance](WebViewControllerCommand command) {
              return RunCommand(weak_instance, std::move(command));
            }
        );
      },
      .disconnect = [](WebPlatformViewInstance&, const WebViewController& controller) {
        WebViewControllerAccess::Disconnect(controller);
      },
  };

  root.RegisterPlatformView<WebViewProperties, WebViewController>(web_view_type, std::move(factory));
}

} // namespace huxerui::detail

extern "C" EMSCRIPTEN_KEEPALIVE void huxerui_webview_did_load(std::uint32_t identity) {
  huxerui::detail::DidLoad(identity);
}

extern "C" EMSCRIPTEN_KEEPALIVE void huxerui_webview_did_receive_message(
    std::uint32_t identity,
    const char* message
) {
  huxerui::detail::DidReceiveMessage(identity, message);
}
