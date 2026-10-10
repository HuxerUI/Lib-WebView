#include <huxerui/webview.h>
#include "detail/webview_internal.h"
#include <cstdio>
#include <variant>

int main() {
  using namespace huxerui;
  WebViewController controller;
  int failures = 0, queries = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::printf("FAIL %s\n", message); ++failures; }
  };
  bool completed = false;
  auto completion = [&](PlatformResult<std::string> result) {
    completed = std::holds_alternative<std::string>(result) &&
        std::get<std::string>(result) == "session=secret";
  };
  check(!controller.GetCookies("https://example.com/api", completion), "disconnected query rejected");
  detail::WebViewControllerAccess::Connect(controller, [&](detail::WebViewControllerCommand command) {
    auto* cookies = std::get_if<detail::WebViewCookieCommand>(&command);
    check(cookies != nullptr, "typed cookie command");
    if (!cookies) return false;
    ++queries;
    if (cookies->clear) {
      check(cookies->url.empty(), "clear all has no URL filter");
      cookies->completion(std::string{});
    } else {
      check(cookies->url == "https://example.com/api", "cookie URL retained");
      cookies->completion(std::string("session=secret"));
    }
    return true;
  });
  check(!controller.GetCookies("", completion), "empty URL rejected");
  check(!controller.GetCookies("https://example.com/api", {}), "missing completion rejected");
  check(!controller.ClearCookies({}), "missing clear completion rejected");
  check(controller.GetCookies("https://example.com/api", completion) && completed, "query returns result");
  bool cleared = false;
  check(controller.ClearCookies([&](PlatformResult<std::string> result) {
    cleared = std::holds_alternative<std::string>(result) && std::get<std::string>(result).empty();
  }) && cleared, "clear completes with empty string");
  detail::WebViewControllerAccess::Disconnect(controller);
  check(!controller.ClearCookies(completion) && queries == 2, "disconnect prevents commands");
  return failures ? 1 : 0;
}
