#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

#include <huxerui/event.h>
#include <huxerui/platform_registry.h>
#include <huxerui/root.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

/// @file
/// Portable embedded web content for HuxerUI applications.
///
/// Install the library with `InstallWebView`, retain commands with
/// `UseWebViewController`, and keep the latest navigation snapshot in
/// application state:
/// @code
/// auto navigation = UseState(WebViewNavigationState{});
/// auto controller = UseWebViewController();
///
/// return WebView({
///     .url = "https://example.com",
///     .java_script_enabled = true,
/// }, controller)
///     .On<WebViewEvents::NavigationChanged>([navigation](const WebViewNavigationState& state) {
///       navigation = state;
///     })
///     .With(Frame{.min_height = 320.0F});
/// @endcode
///
/// A WebView is a PlatformView leaf and therefore needs bounded layout. Native
/// focus, input, accessibility, clipping, and composition follow the active
/// platform's PlatformView capabilities.

namespace huxerui {

/// Describes an imperative top-level web request.
///
/// `url` remains an exact owned UTF-8 string. Method, header, and body support
/// depends on the platform web engine. Use `WebViewController::LoadUrl` for a
/// portable GET without custom headers or a body.
struct WebViewRequest {
  /// The destination URL.
  std::string url;

  /// The HTTP method. The controller normalizes ASCII letters to uppercase.
  std::string method = "GET";

  /// Additional HTTP request headers.
  std::map<std::string, std::string, std::less<>> headers;

  /// Optional UTF-8 request body.
  std::optional<std::string> body;

  bool operator==(const WebViewRequest&) const = default;

  /// Encodes the value for a cross-language PlatformView adapter.
  [[nodiscard]] static PlatformPayload Encode(const WebViewRequest& value);
};

/// Describes HTML content loaded directly into the WebView.
struct WebViewHtml {
  /// The complete HTML document or fragment.
  std::string content;

  /// The optional base URL used to resolve relative URLs. An empty value uses
  /// the platform's blank-document origin.
  std::string base_url;

  bool operator==(const WebViewHtml&) const = default;

  /// Encodes the value for a cross-language PlatformView adapter.
  [[nodiscard]] static PlatformPayload Encode(const WebViewHtml& value);
};

/// The complete declarative configuration of a WebView.
///
/// `url` is controlled application state. A changed value starts a new
/// top-level load, while an equal value does not reload during recomposition.
/// When the application wants an in-page navigation to become the new
/// controlled value, write `WebViewNavigationState::url` back to the state used
/// to construct these properties.
struct WebViewProperties {
  /// The top-level URL to load as an exact owned UTF-8 string.
  std::string url;

  /// Enables page JavaScript. Disabled by default.
  bool java_script_enabled = false;

  bool operator==(const WebViewProperties&) const = default;

  /// Encodes the value for a cross-language PlatformView adapter.
  [[nodiscard]] static PlatformPayload Encode(const WebViewProperties& value);
};

/// A navigation that is about to be handled by the platform web engine.
struct WebViewNavigationRequest {
  /// The requested URL as reported by the platform.
  std::string url;

  /// Whether the navigation targets the main document frame.
  bool is_main_frame = true;

  bool operator==(const WebViewNavigationRequest&) const = default;

  /// Decodes a navigation request emitted by a cross-language adapter.
  [[nodiscard]] static WebViewNavigationRequest Decode(const PlatformPayload& payload);
};

/// A coherent snapshot of observable WebView navigation state.
///
/// Store this value in application `State` when it drives UI such as Back and
/// Forward button availability, page title, or a progress indicator. Some
/// browser-hosted cross-origin content exposes only approximate state; unknown
/// strings are empty and unavailable history actions are `false`.
struct WebViewNavigationState {
  /// The current top-level URL known to the platform.
  std::string url;

  /// The current document title, or an empty string when unavailable.
  std::string title;

  /// Whether a top-level navigation is currently loading.
  bool is_loading = false;

  /// Estimated load progress in the inclusive range from `0.0` to `1.0`.
  double progress = 0.0;

  /// Whether `WebViewController::GoBack` can currently navigate history.
  bool can_go_back = false;

  /// Whether `WebViewController::GoForward` can currently navigate history.
  bool can_go_forward = false;

  bool operator==(const WebViewNavigationState&) const = default;

  /// Decodes a navigation snapshot emitted by a cross-language adapter.
  [[nodiscard]] static WebViewNavigationState Decode(const PlatformPayload& payload);
};

/// Describes a WebView load failure.
struct WebViewLoadError {
  /// A diagnostic identifier. Platform-prefixed codes must not be used as a
  /// portable error classification.
  std::string code;

  /// A human-readable diagnostic message.
  std::string message;

  /// The related URL when the platform can report it.
  std::optional<std::string> url;

  bool operator==(const WebViewLoadError&) const = default;

  /// Decodes a load error emitted by a cross-language adapter.
  [[nodiscard]] static WebViewLoadError Decode(const PlatformPayload& payload);
};

/// Typed events emitted by a mounted WebView.
struct WebViewEvents {
  /// Synchronously asks whether a navigation may proceed.
  ///
  /// Return `false` to cancel the request. A missing handler allows navigation.
  /// The handler runs on the WebView's owning UI thread and must return without
  /// starting asynchronous decision work.
  struct NavigationRequested : Event<bool(const WebViewNavigationRequest&)> {
    static constexpr std::string_view Name = "navigationRequested";
  };

  /// Reports the latest coherent navigation snapshot.
  ///
  /// The event is emitted when observable URL, title, loading, progress, or
  /// history availability changes. Platforms may coalesce native updates.
  struct NavigationChanged : Event<void(const WebViewNavigationState&)> {
    static constexpr std::string_view Name = "navigationChanged";
  };

  /// Reports that a top-level load started.
  struct LoadStarted : Event<void(const WebViewNavigationState&)> {
    static constexpr std::string_view Name = "loadStarted";
  };

  /// Reports that a top-level load completed successfully.
  struct LoadFinished : Event<void(const WebViewNavigationState&)> {
    static constexpr std::string_view Name = "loadFinished";
  };

  /// Reports that a top-level load failed.
  struct LoadFailed : Event<void(const WebViewLoadError&)> {
    static constexpr std::string_view Name = "loadFailed";
  };

  /// Delivers text posted by page JavaScript through `huxerui.postMessage`.
  ///
  /// Treat every message as untrusted input. Availability depends on page
  /// JavaScript being enabled and on the active platform's content policy.
  struct MessageReceived : Event<void(const std::string&)> {
    static constexpr std::string_view Name = "messageReceived";
  };
};

/// Completion invoked by `WebViewController::EvaluateJavaScript`.
///
/// Successful values contain the platform's JSON representation of the script
/// result. Failures contain a `PlatformError`.
using WebViewJavaScriptCompletion = std::function<void(PlatformResult<std::string>)>;

class WebViewController;

namespace detail {

class WebViewControllerState;
struct WebViewControllerAccess;

} // namespace detail

/// An imperative command handle for one mounted WebView.
///
/// Create and retain this value with `UseWebViewController`:
/// @code
/// auto controller = UseWebViewController();
///
/// return Column {
///   Button("Open documentation").OnClick([controller] {
///     controller.LoadUrl("https://example.com/docs");
///   }),
///   WebView({.url = "about:blank"}, controller).With(Grow()),
/// };
/// @endcode
class WebViewController {
public:
  /// Creates a disconnected controller.
  WebViewController();

  /// Returns whether this controller is currently bound to a mounted WebView.
  [[nodiscard]] bool IsConnected() const noexcept;

  /// Requests a reload of the current document.
  bool Reload() const;

  /// Requests cancellation of the current load.
  bool StopLoading() const;

  /// Navigates to the previous entry when WebView history allows it.
  bool GoBack() const;

  /// Navigates to the next entry when WebView history allows it.
  bool GoForward() const;

  /// Loads a URL using a GET request without custom headers or a body.
  ///
  /// This is equivalent to `LoadRequest({.url = url})`.
  bool LoadUrl(std::string url) const;

  /// Loads an explicit request using the capabilities of the platform engine.
  bool LoadRequest(WebViewRequest request) const;

  /// Loads HTML content directly into the current WebView.
  bool LoadHtml(WebViewHtml html) const;

  /// Evaluates JavaScript in the current document and reports its result.
  bool EvaluateJavaScript(std::string script, WebViewJavaScriptCompletion completion) const;

  bool operator==(const WebViewController&) const = default;

private:
  std::shared_ptr<detail::WebViewControllerState> state_;

  friend struct detail::WebViewControllerAccess;
};

/// Retains a WebView controller in the current composition scope.
///
/// One controller may be bound to at most one mounted WebView at a time. Copies
/// refer to the same retained controller state. Commands return `false` when no
/// compatible live WebView accepts them; `true` means accepted, not completed.
inline WebViewController UseWebViewController(
    const std::source_location& location = std::source_location::current()
) {
  return UseState(WebViewController{}, location);
}

/// Creates a WebView without an imperative controller.
View WebView(WebViewProperties properties);

/// Creates a WebView and binds a retained imperative controller.
View WebView(WebViewProperties properties, WebViewController controller);

/// Registers the WebView PlatformView implementation for the active platform.
///
/// Add this function to `AppOptions::root_hooks` before mounting any WebView:
/// @code
/// const Application application{
///     App,
///     {
///         .root_hooks = {
///             huxerui::InstallWebView,
///         },
///     },
/// };
/// @endcode
///
/// Installation throws when the current platform has no WebView adapter.
void InstallWebView(RootContext& root);

} // namespace huxerui
