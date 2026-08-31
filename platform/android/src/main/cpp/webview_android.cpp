#include <huxerui/android/platform_registry.h>
#include <huxerui/webview.h>

#include <string>
#include <utility>
#include <variant>

#include "detail/webview_internal.h"

namespace huxerui::detail {

namespace {

std::string CommandName(WebViewCommand command) {
  switch (command) {
    case WebViewCommand::Reload:
      return "reload";
    case WebViewCommand::StopLoading:
      return "stopLoading";
    case WebViewCommand::GoBack:
      return "goBack";
    case WebViewCommand::GoForward:
      return "goForward";
  }
}

bool RunCommand(PlatformChannel channel, WebViewControllerCommand command) {
  if (!channel.IsOpen()) {
    return false;
  }
  if (const auto* value = std::get_if<WebViewCommand>(&command)) {
    channel.Invoke<std::monostate>(
        CommandName(*value),
        [](PlatformResult<std::monostate>) {}
    );
    return true;
  }
  if (const auto* value = std::get_if<WebViewLoadRequestCommand>(&command)) {
    channel.Invoke<std::monostate>(
        "loadRequest",
        value->request,
        [](PlatformResult<std::monostate>) {}
    );
    return true;
  }
  if (const auto* value = std::get_if<WebViewLoadHtmlCommand>(&command)) {
    channel.Invoke<std::monostate>(
        "loadHtml",
        value->html,
        [](PlatformResult<std::monostate>) {}
    );
    return true;
  }
  auto value = std::get<WebViewEvaluateJavaScriptCommand>(std::move(command));
  channel.Invoke<std::string>(
      "evaluateJavaScript",
      value.script,
      std::move(value.completion)
  );
  return true;
}

} // namespace

void InstallPlatformWebView(RootContext& root) {
  android::JavaPlatformViewFactory<WebViewProperties, WebViewController> factory{
      .class_name = "org.huxerui.lib.webview.HuxerUIWebView$Factory",
      .connect = [](const WebViewController& controller, PlatformChannel channel) {
        WebViewControllerAccess::Connect(
            controller,
            [channel = std::move(channel)](WebViewControllerCommand command) {
              return RunCommand(channel, std::move(command));
            }
        );
      },
      .disconnect = [](const WebViewController& controller) {
        WebViewControllerAccess::Disconnect(controller);
      },
  };

  root.RegisterPlatformView<WebViewProperties, WebViewController>(web_view_type, std::move(factory));
}

} // namespace huxerui::detail
