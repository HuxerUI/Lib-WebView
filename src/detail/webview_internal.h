#pragma once

#include <functional>
#include <variant>

#include <huxerui/webview.h>

#define WEBVIEW_UNUSED(name) static_cast<void>(name)

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

struct WebViewGetCookiesCommand {
  std::string url;
  WebViewJavaScriptCompletion completion;
};

struct WebViewClearCookiesCommand {
  WebViewJavaScriptCompletion completion;
};

struct WebViewSetCookieCommand {
  std::string url;
  WebViewCookie cookie;
  WebViewJavaScriptCompletion completion;
};

struct WebViewDeleteCookieCommand {
  std::string url;
  std::string name;
  std::string domain;
  std::string path;
  WebViewJavaScriptCompletion completion;
};

struct WebViewGetCookieListCommand {
  std::string url;
  WebViewCookieListCompletion completion;
};

using WebViewControllerCommand = std::variant<
    WebViewCommand,
    WebViewLoadRequestCommand,
    WebViewLoadHtmlCommand,
    WebViewEvaluateJavaScriptCommand,
    WebViewGetCookiesCommand,
    WebViewClearCookiesCommand,
    WebViewSetCookieCommand,
    WebViewDeleteCookieCommand,
    WebViewGetCookieListCommand
>;

using WebViewCommandHandler = std::function<bool(WebViewControllerCommand)>;

struct WebViewControllerAccess {
  static void Connect(const WebViewController& controller, WebViewCommandHandler handler);
  static void Disconnect(const WebViewController& controller);
};

void InstallPlatformWebView(ApplicationContext& root);

} // namespace huxerui::detail
