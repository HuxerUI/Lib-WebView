#include <huxerui/webview.h>

#include <algorithm>
#include <cctype>
#include <utility>

#include "detail/webview_internal.h"

namespace huxerui::detail {

class WebViewControllerState {
public:
  WebViewCommandHandler handler;
};

void WebViewControllerAccess::Connect(
    const WebViewController& controller,
    WebViewCommandHandler handler
) {
  controller.state_->handler = std::move(handler);
}

void WebViewControllerAccess::Disconnect(const WebViewController& controller) {
  controller.state_->handler = {};
}

} // namespace huxerui::detail

namespace huxerui {

WebViewController::WebViewController()
    : state_(std::make_shared<detail::WebViewControllerState>()) {}

bool WebViewController::IsConnected() const noexcept {
  return state_ && static_cast<bool>(state_->handler);
}

namespace {

std::string UppercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return value;
}

bool RunCommand(
    const std::shared_ptr<detail::WebViewControllerState>& state,
    detail::WebViewControllerCommand command
) {
  return state && state->handler && state->handler(std::move(command));
}

} // namespace

bool WebViewController::Reload() const {
  return RunCommand(state_, detail::WebViewCommand::Reload);
}

bool WebViewController::StopLoading() const {
  return RunCommand(state_, detail::WebViewCommand::StopLoading);
}

bool WebViewController::GoBack() const {
  return RunCommand(state_, detail::WebViewCommand::GoBack);
}

bool WebViewController::GoForward() const {
  return RunCommand(state_, detail::WebViewCommand::GoForward);
}

bool WebViewController::LoadUrl(std::string url) const {
  return LoadRequest({.url = std::move(url)});
}

bool WebViewController::LoadRequest(WebViewRequest request) const {
  request.method = UppercaseAscii(std::move(request.method));
  return RunCommand(state_, detail::WebViewLoadRequestCommand{std::move(request)});
}

bool WebViewController::LoadHtml(WebViewHtml html) const {
  return RunCommand(state_, detail::WebViewLoadHtmlCommand{std::move(html)});
}

bool WebViewController::EvaluateJavaScript(
    std::string script,
    WebViewJavaScriptCompletion completion
) const {
  if (!completion) {
    return false;
  }
  return RunCommand(
      state_,
      detail::WebViewEvaluateJavaScriptCommand{
          .script = std::move(script),
          .completion = std::move(completion),
      }
  );
}

} // namespace huxerui
