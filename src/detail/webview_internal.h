#pragma once

#include <functional>
#include <variant>

#include <huxerui/webview.h>

namespace huxerui::detail {

inline constexpr char web_view_type[] = "huxerui/webview";

enum class WebViewCommand {
  Reload,
  StopLoading,
  GoBack,
  GoForward,
};

struct WebViewLoadRequestCommand {
  WebViewRequest request;
};

struct WebViewLoadHtmlCommand {
  WebViewHtml html;
};

struct WebViewEvaluateJavaScriptCommand {
  std::string script;
  WebViewJavaScriptCompletion completion;
};

struct WebViewCookieCommand {
  std::string url;
  bool clear = false;
  WebViewJavaScriptCompletion completion;
};

using WebViewControllerCommand = std::variant<
    WebViewCommand,
    WebViewLoadRequestCommand,
    WebViewLoadHtmlCommand,
    WebViewEvaluateJavaScriptCommand,
    WebViewCookieCommand
>;

using WebViewCommandHandler = std::function<bool(WebViewControllerCommand)>;

struct WebViewControllerAccess {
  static void Connect(const WebViewController& controller, WebViewCommandHandler handler);
  static void Disconnect(const WebViewController& controller);
};

void InstallPlatformWebView(ApplicationContext& root);

} // namespace huxerui::detail
