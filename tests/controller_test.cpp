#include <huxerui/webview.h>
#include "detail/webview_internal.h"
#include <cstdio>
#include <variant>

int main() {
  using namespace huxerui;
  WebViewController controller;
  int failures = 0, queries = 0, clears = 0;
  auto check = [&](bool passed, const char* message) {
    if (!passed) { std::printf("FAIL %s\n", message); ++failures; }
  };
  const WebViewCookie cookie{
      .name = "session",
      .value = "secret",
      .domain = "example.com",
      .path = "/api",
      .expires = std::chrono::system_clock::time_point(std::chrono::seconds(1700000000)),
      .secure = true,
      .http_only = true,
      .same_site = WebViewCookieSameSite::Strict,
  };
  check(WebViewCookie::Decode(WebViewCookie::Encode(cookie)) == cookie, "structured cookie round trip");
  bool completed = false;
  auto completion = [&](PlatformResult<std::string> result) {
    completed = std::holds_alternative<std::string>(result) &&
        std::get<std::string>(result) == "session=secret";
  };
  auto empty_completion = [&](PlatformResult<std::string> result) {
    completed = std::holds_alternative<std::string>(result) && std::get<std::string>(result).empty();
  };
  bool listed = false;
  auto list_completion = [&](PlatformResult<std::vector<WebViewCookie>> result) {
    listed = std::holds_alternative<std::vector<WebViewCookie>>(result) &&
        std::get<std::vector<WebViewCookie>>(result) == std::vector<WebViewCookie>{cookie};
  };
  check(!controller.GetCookies("https://example.com/api", completion), "disconnected query rejected");
  check(!controller.SetCookie("https://example.com/api", cookie, empty_completion), "disconnected set rejected");
  check(!controller.DeleteCookie("https://example.com/api", "session", "example.com", "/api", empty_completion),
        "disconnected delete rejected");
  check(!controller.GetCookieList("https://example.com/api", list_completion), "disconnected list rejected");
  check(!controller.ClearCookies(empty_completion), "disconnected clear rejected");
  int writes = 0, deletions = 0, lists = 0;
  detail::WebViewControllerAccess::Connect(controller, [&](detail::WebViewControllerCommand command) {
    if (auto* set = std::get_if<detail::WebViewSetCookieCommand>(&command)) {
      ++writes;
      check(set->url == "https://example.com/api" && set->cookie == cookie, "set command retains cookie");
      set->completion(std::string{});
      return true;
    }
    if (auto* deletion = std::get_if<detail::WebViewDeleteCookieCommand>(&command)) {
      ++deletions;
      check(deletion->url == "https://example.com/api" && deletion->name == "session" &&
            deletion->domain == "example.com" && deletion->path == "/api", "delete command retains identity");
      deletion->completion(std::string{});
      return true;
    }
    if (auto* list = std::get_if<detail::WebViewGetCookieListCommand>(&command)) {
      ++lists;
      check(list->url == "https://example.com/api", "list command retains URL");
      list->completion(std::vector<WebViewCookie>{cookie});
      return true;
    }
    if (auto* cookies = std::get_if<detail::WebViewGetCookiesCommand>(&command)) {
      ++queries;
      check(cookies->url == "https://example.com/api", "cookie URL retained");
      cookies->completion(std::string("session=secret"));
      return true;
    }
    auto* clear = std::get_if<detail::WebViewClearCookiesCommand>(&command);
    check(clear != nullptr, "typed clear cookies command");
    if (!clear) return false;
    ++clears;
    clear->completion(std::string{});
    return true;
  });
  check(!controller.GetCookies("", completion), "empty URL rejected");
  check(!controller.GetCookies("https://example.com/api", {}), "missing completion rejected");
  check(!controller.ClearCookies({}), "missing clear completion rejected");
  check(!controller.SetCookie("", cookie, empty_completion), "set requires URL");
  check(!controller.SetCookie("https://example.com/api", WebViewCookie{.domain = "example.com"}, empty_completion),
        "set requires name");
  check(!controller.SetCookie("https://example.com/api", WebViewCookie{.name = "session"}, empty_completion),
        "set requires domain");
  WebViewCookie insecure_none = cookie;
  insecure_none.same_site = WebViewCookieSameSite::None;
  insecure_none.secure = false;
  check(!controller.SetCookie("https://example.com/api", insecure_none, empty_completion),
        "SameSite None requires Secure");
  WebViewCookie invalid_path = cookie;
  invalid_path.path = "api";
  check(!controller.SetCookie("https://example.com/api", invalid_path, empty_completion), "set requires absolute path");
  WebViewCookie invalid_text = cookie;
  invalid_text.value = "secret; Secure";
  check(!controller.SetCookie("https://example.com/api", invalid_text, empty_completion), "set rejects header injection");
  check(!controller.SetCookie("https://example.com/api", cookie, {}), "set requires completion");
  check(!controller.DeleteCookie("", "session", "example.com", "/api", empty_completion), "delete requires URL");
  check(!controller.DeleteCookie("https://example.com/api", "", "example.com", "/api", empty_completion),
        "delete requires name");
  check(!controller.DeleteCookie("https://example.com/api", "session", "", "/api", empty_completion),
        "delete requires domain");
  check(!controller.DeleteCookie("https://example.com/api", "session", "example.com", "api", empty_completion),
        "delete requires absolute path");
  check(!controller.DeleteCookie("https://example.com/api", "session", "example.com", "/api", {}),
        "delete requires completion");
  check(!controller.GetCookieList("", list_completion), "list requires URL");
  check(!controller.GetCookieList("https://example.com/api", {}), "list requires completion");
  check(controller.GetCookies("https://example.com/api", completion) && completed && queries == 1 && clears == 0,
        "query returns result without clearing cookies");
  bool cleared = false;
  check(controller.ClearCookies([&](PlatformResult<std::string> result) {
    cleared = std::holds_alternative<std::string>(result) && std::get<std::string>(result).empty();
  }) && cleared && clears == 1 && queries == 1, "clear completes without querying cookies");
  completed = false;
  check(controller.SetCookie("https://example.com/api", cookie, empty_completion) && completed && writes == 1,
        "set completes with empty string");
  completed = false;
  check(controller.DeleteCookie("https://example.com/api", "session", "example.com", "/api", empty_completion) &&
        completed && deletions == 1, "delete completes with empty string");
  check(controller.GetCookieList("https://example.com/api", list_completion) && listed && lists == 1,
        "list returns structured cookies");
  detail::WebViewControllerAccess::Disconnect(controller);
  check(!controller.ClearCookies(completion) && clears == 1, "disconnect prevents clear");
  check(!controller.GetCookies("https://example.com/api", completion) && queries == 1,
        "disconnect prevents query");
  check(!controller.SetCookie("https://example.com/api", cookie, empty_completion) && writes == 1,
        "disconnect prevents set");
  check(!controller.DeleteCookie("https://example.com/api", "session", "example.com", "/api", empty_completion) &&
        deletions == 1, "disconnect prevents delete");
  check(!controller.GetCookieList("https://example.com/api", list_completion) && lists == 1,
        "disconnect prevents list");
  return failures ? 1 : 0;
}
