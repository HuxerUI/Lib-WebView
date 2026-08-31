#include <huxerui/webview.h>

#include <stdexcept>
#include <utility>

#include "detail/webview_internal.h"

namespace huxerui {

PlatformPayload WebViewRequest::Encode(const WebViewRequest& value) {
  PlatformPayload::Object headers;
  for (const auto& [name, content] : value.headers) {
    headers.emplace(name, content);
  }
  return PlatformPayload::Object{
      {"body", value.body.has_value() ? PlatformPayload(*value.body) : PlatformPayload{}},
      {"headers", std::move(headers)},
      {"method", value.method},
      {"url", value.url},
  };
}

PlatformPayload WebViewHtml::Encode(const WebViewHtml& value) {
  return PlatformPayload::Object{
      {"baseUrl", value.base_url},
      {"content", value.content},
  };
}

PlatformPayload WebViewProperties::Encode(const WebViewProperties& value) {
  return PlatformPayload::Object{
      {"javaScriptEnabled", value.java_script_enabled},
      {"url", value.url},
  };
}

WebViewNavigationRequest WebViewNavigationRequest::Decode(const PlatformPayload& payload) {
  const PlatformPayload::Object& fields = payload.AsObject();
  return {
      .url = std::string(fields.at("url").AsString()),
      .is_main_frame = fields.at("isMainFrame").AsBoolean(),
  };
}

WebViewNavigationState WebViewNavigationState::Decode(const PlatformPayload& payload) {
  const PlatformPayload::Object& fields = payload.AsObject();
  return {
      .url = std::string(fields.at("url").AsString()),
      .title = std::string(fields.at("title").AsString()),
      .is_loading = fields.at("isLoading").AsBoolean(),
      .progress = fields.at("progress").AsDouble(),
      .can_go_back = fields.at("canGoBack").AsBoolean(),
      .can_go_forward = fields.at("canGoForward").AsBoolean(),
  };
}

WebViewLoadError WebViewLoadError::Decode(const PlatformPayload& payload) {
  const PlatformPayload::Object& fields = payload.AsObject();
  const PlatformPayload& url = fields.at("url");
  return {
      .code = std::string(fields.at("code").AsString()),
      .message = std::string(fields.at("message").AsString()),
      .url = url.IsNull() ? std::nullopt : std::optional<std::string>{std::string(url.AsString())},
  };
}

View WebView(WebViewProperties properties) {
  return PlatformView(detail::web_view_type, std::move(properties));
}

View WebView(WebViewProperties properties, WebViewController controller) {
  return PlatformView(detail::web_view_type, std::move(properties)).Controller(std::move(controller));
}

void InstallWebView(RootContext& root) {
#if defined(_WIN32) || defined(__ANDROID__) || defined(__APPLE__) || defined(__EMSCRIPTEN__)
  detail::InstallPlatformWebView(root);
#else
  static_cast<void>(root);
  throw std::logic_error("HuxerUI WebView is not available on this platform");
#endif
}

} // namespace huxerui
