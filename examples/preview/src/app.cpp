#include <app_resources.h>
#include <huxerui/huxerui.h>
#include <huxerui/webview.h>

using namespace huxerui;

[[huxerui::composable]]
View WebContent() {
  auto address = UseState(TextEditingValue::FromText("https://example.com"));
  auto requested_url = UseState(std::string("https://example.com"));
  auto java_script_enabled = UseState(false);
  auto navigation = UseState(WebViewNavigationState{
      .url = requested_url,
  });
  auto error = UseState(std::string{});
  auto lifecycle = UseState(std::string("Idle"));
  auto java_script_result = UseState(std::string{});
  auto message = UseState(std::string{});
  WebViewController controller = UseWebViewController();

  auto load_url = [address, controller] {
    controller.LoadUrl(address->text);
  };

  const ThemeSpec& theme = UseTheme();
  const std::string lifecycle_text = lifecycle;
  const std::string error_text = error;
  const std::string message_text = message;
  const std::string java_script_result_text = java_script_result;

  View loading_indicator = Divider().With(Frame{.height = 3.0F});
  if (navigation->is_loading) {
    loading_indicator = ProgressBar(static_cast<float>(navigation->progress)).With(Frame{.height = 3.0F});
  }

  return Column {
    Row {
      IconButton(app::images::arrow_back, "Back")
          .OnClick([controller] {
            controller.GoBack();
          })
          .With(Enabled(navigation->can_go_back), Tooltip("Back")),
      IconButton(app::images::arrow_forward, "Forward")
          .OnClick([controller] {
            controller.GoForward();
          })
          .With(Enabled(navigation->can_go_forward), Tooltip("Forward")),
      IconButton(
          navigation->is_loading ? app::images::stop : app::images::refresh,
          navigation->is_loading ? "Stop loading" : "Reload"
      )
          .OnClick([controller, navigation] {
            if (navigation->is_loading) {
              controller.StopLoading();
            } else {
              controller.Reload();
            }
          })
          .With(Tooltip(navigation->is_loading ? "Stop loading" : "Reload")),
      TextField(address)
          .Placeholder("Search or enter address")
          .Variant(TextFieldVariant::Outlined)
          .InputConfiguration({
              .type = TextInputType::Url,
              .action = TextInputAction::Go,
          })
          .OnChanged([address](const TextEditingValue& value) {
            address = value;
          })
          .OnSubmitted(load_url)
          .With(Grow()),
      IconButton(app::images::go, "Go")
          .OnClick(load_url)
          .With(Tooltip("Go")),
    }.With(
        Padding(EdgeInsets::Symmetric(8.0F, 6.0F)),
        Spacing(4.0F),
        CrossAlign(CrossAxisAlignment::Center),
        Background(theme.colors.surface_container_highest)
    ),
    loading_indicator,
    WebView({
          .url = requested_url,
          .java_script_enabled = java_script_enabled,
        }, controller)
        .On<WebViewEvents::NavigationRequested>([](const WebViewNavigationRequest&) {
          return true;
        })
        .On<WebViewEvents::NavigationChanged>(
            [address, navigation, requested_url](const WebViewNavigationState& state) {
              if (state.url != navigation->url) {
                address = TextEditingValue::FromText(state.url);
              }
              navigation = state;
              requested_url = state.url;
            }
        )
        .On<WebViewEvents::LoadStarted>([lifecycle, error](const WebViewNavigationState&) {
          lifecycle = "Loading";
          error = {};
        })
        .On<WebViewEvents::LoadFinished>([lifecycle](const WebViewNavigationState&) {
          lifecycle = "Finished";
        })
        .On<WebViewEvents::LoadFailed>([error, lifecycle](const WebViewLoadError& value) {
          error = value.message;
          lifecycle = "Failed";
        })
        .On<WebViewEvents::MessageReceived>([message](const std::string& value) {
          message = value;
        })
        .With(Grow(), Frame{.min_height = 240.0F}, ClipChildren()),
    Divider(),
    Column {
      Row {
        Text("WebView API", TextRole::Title),
        Spacer(),
        Switch("JavaScript", java_script_enabled).OnChanged([java_script_enabled](bool enabled) {
          java_script_enabled = enabled;
        }),
      }.With(CrossAlign(CrossAxisAlignment::Center)),
      Flow {
        Button("Request").OnClick([controller, address] {
          controller.LoadRequest({
              .url = address->text,
              .headers = {{"X-HuxerUI", "WebView Preview"}},
          });
        }),
        Button("HTML").OnClick([controller] {
          controller.LoadHtml({
              .content = "<html><body><h1>HuxerUI WebView</h1>"
                         "<button onclick=\"huxerui.postMessage('Hello from JavaScript')\">"
                         "Send message</button></body></html>",
              .base_url = "https://example.com/",
          });
        }),
        Button("Run JS").OnClick([controller, java_script_result] {
          controller.EvaluateJavaScript(
              "({title: document.title, url: location.href})",
              [java_script_result](PlatformResult<std::string> result) {
                if (const auto* value = std::get_if<std::string>(&result)) {
                  java_script_result = *value;
                } else {
                  java_script_result = std::get<PlatformError>(result).message;
                }
              }
          );
        }),
      }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
      Text(message_text.empty() ? "Message: None" : "Message: " + message_text),
      Text(
          java_script_result_text.empty() ? "JavaScript result: None"
                                          : "JavaScript result: " + java_script_result_text
      ),
    }.With(
        Padding(EdgeInsets::Symmetric(12.0F, 8.0F)),
        Spacing(8.0F),
        CrossAlign(CrossAxisAlignment::Stretch),
        Background(theme.colors.surface_container)
    ),
    Row {
      Text(
          error_text.empty() ? (navigation->title.empty() ? "No page title" : navigation->title)
                             : "Error: " + error_text
      ).With(Grow()),
      Text(lifecycle_text),
    }.With(
        Padding(EdgeInsets::Symmetric(12.0F, 6.0F)),
        Spacing(12.0F),
        CrossAlign(CrossAxisAlignment::Center),
        Background(theme.colors.surface_container_highest),
        Foreground(theme.colors.on_surface_variant),
        FontSize(theme.typography.body_small)
    ),
  }.With(
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.background)
  );
}

View App() {
  return FlatTheme(WebContent());
}

const Application application{
    App,
    {
        .window = {
            .title = "WebView Preview",
        },
        .root_hooks = {
            huxerui::InstallWebView,
        },
    }
};
