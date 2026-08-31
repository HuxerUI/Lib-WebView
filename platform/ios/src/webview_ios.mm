#import <WebKit/WebKit.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <huxerui/webview.h>

#include <huxerui/ios/platform_registry.h>

#include "detail/webview_internal.h"

@class HUXIOSWebViewNavigationDelegate;

namespace huxerui::detail {

struct IOSWebViewInstance : std::enable_shared_from_this<IOSWebViewInstance> {
  WKWebView* view = nil;
  HUXIOSWebViewNavigationDelegate* navigation_delegate = nil;
  WebViewProperties properties;
  PlatformEventEmitter events;
  bool disposed = false;
  bool observing_progress = false;
};

} // namespace huxerui::detail

@interface HUXIOSWebViewNavigationDelegate : NSObject <WKNavigationDelegate, WKScriptMessageHandler> {
 @private
  std::weak_ptr<huxerui::detail::IOSWebViewInstance> instance_;
}

- (instancetype)initWithInstance:(std::weak_ptr<huxerui::detail::IOSWebViewInstance>)instance;

@end

namespace huxerui::detail {

namespace {

std::string ToString(NSString* value) {
  if (value == nil) {
    return {};
  }
  const char* characters = value.UTF8String;
  return characters == nullptr ? std::string{} : std::string(characters);
}

NSString* ToNSString(const std::string& value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

std::string UppercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return value;
}

void EmitNavigationState(const std::shared_ptr<IOSWebViewInstance>& instance, bool loading) {
  if (!instance || instance->disposed || instance->view == nil) {
    return;
  }

  WKWebView* view = instance->view;
  instance->events.Emit<WebViewEvents::NavigationChanged>(WebViewNavigationState{
      .url = ToString(view.URL.absoluteString),
      .title = ToString(view.title),
      .is_loading = loading,
      .progress = loading ? view.estimatedProgress : 1.0,
      .can_go_back = view.canGoBack,
      .can_go_forward = view.canGoForward,
  });
}

WebViewNavigationState NavigationState(
    const std::shared_ptr<IOSWebViewInstance>& instance,
    bool loading
) {
  WKWebView* view = instance->view;
  return {
      .url = ToString(view.URL.absoluteString),
      .title = ToString(view.title),
      .is_loading = loading,
      .progress = loading ? view.estimatedProgress : 1.0,
      .can_go_back = view.canGoBack,
      .can_go_forward = view.canGoForward,
  };
}

template <class Event>
void EmitStateEvent(const std::shared_ptr<IOSWebViewInstance>& instance, bool loading) {
  if (!instance || instance->disposed || instance->view == nil) {
    return;
  }
  instance->events.Emit<Event>(NavigationState(instance, loading));
}

void EmitLoadError(const std::shared_ptr<IOSWebViewInstance>& instance, NSError* error, NSString* url) {
  if (!instance || instance->disposed || error == nil || error.code == NSURLErrorCancelled) {
    return;
  }

  instance->events.Emit<WebViewEvents::LoadFailed>(WebViewLoadError{
      .code = ToString(error.domain) + "/" + std::to_string(error.code),
      .message = ToString(error.localizedDescription),
      .url = url == nil ? std::nullopt : std::optional<std::string>{ToString(url)},
  });
}

void LoadUrl(const std::shared_ptr<IOSWebViewInstance>& instance, const std::string& value) {
  if (!instance || instance->disposed || instance->view == nil) {
    return;
  }

  NSString* string = ToNSString(value);
  NSURL* url = string == nil ? nil : [NSURL URLWithString:string];
  if (url != nil) {
    [instance->view loadRequest:[NSURLRequest requestWithURL:url]];
    return;
  }

  std::weak_ptr<IOSWebViewInstance> weak_instance = instance;
  dispatch_async(dispatch_get_main_queue(), ^{
    std::shared_ptr<IOSWebViewInstance> current = weak_instance.lock();
    if (!current || current->disposed) {
      return;
    }
    current->events.Emit<WebViewEvents::LoadFailed>(WebViewLoadError{
        .code = "webview/invalid-url",
        .message = "The WebView URL is invalid",
        .url = value,
    });
  });
}

void LoadRequest(
    const std::shared_ptr<IOSWebViewInstance>& instance,
    const WebViewRequest& request
) {
  if (!instance || instance->disposed || instance->view == nil) {
    return;
  }

  const std::string method = UppercaseAscii(request.method);
  if (method != "GET" && method != "POST") {
    instance->events.Emit<WebViewEvents::LoadFailed>(WebViewLoadError{
        .code = "webview/unsupported-request",
        .message = "WebView navigation supports only GET and POST requests",
        .url = request.url,
    });
    return;
  }

  NSString* string = ToNSString(request.url);
  NSURL* url = string == nil ? nil : [NSURL URLWithString:string];
  if (url == nil) {
    EmitLoadError(instance, [NSError errorWithDomain:@"webview" code:1 userInfo:@{
      NSLocalizedDescriptionKey: @"The WebView URL is invalid"
    }], string);
    return;
  }

  NSMutableURLRequest* native_request = [NSMutableURLRequest requestWithURL:url];
  native_request.HTTPMethod = ToNSString(method);
  for (const auto& [name, value] : request.headers) {
    [native_request setValue:ToNSString(value) forHTTPHeaderField:ToNSString(name)];
  }
  if (request.body.has_value()) {
    native_request.HTTPBody = [NSData dataWithBytes:request.body->data() length:request.body->size()];
  }
  [instance->view loadRequest:native_request];
}

void LoadHtml(const std::shared_ptr<IOSWebViewInstance>& instance, const WebViewHtml& html) {
  if (!instance || instance->disposed || instance->view == nil) {
    return;
  }
  NSString* content = ToNSString(html.content);
  NSString* base_string = ToNSString(html.base_url);
  NSURL* base_url = html.base_url.empty() || base_string == nil
      ? nil
      : [NSURL URLWithString:base_string];
  if (!html.base_url.empty() && base_url == nil) {
    EmitLoadError(instance, [NSError errorWithDomain:@"webview" code:1 userInfo:@{
      NSLocalizedDescriptionKey: @"The WebView base URL is invalid"
    }], base_string);
    return;
  }
  [instance->view loadHTMLString:content baseURL:base_url];
}

void EvaluateJavaScript(
    const std::shared_ptr<IOSWebViewInstance>& instance,
    WebViewEvaluateJavaScriptCommand command
) {
  if (!instance || instance->disposed || instance->view == nil) {
    command.completion(PlatformError{
        "webview/disconnected",
        "The WebView is disconnected",
        {},
    });
    return;
  }

  WebViewJavaScriptCompletion completion = std::move(command.completion);
  [instance->view evaluateJavaScript:ToNSString(command.script)
                   completionHandler:^(id value, NSError* error) {
    if (error != nil) {
      completion(PlatformError{
          "webview/javascript",
          ToString(error.localizedDescription),
          {},
      });
      return;
    }
    if (value == nil) {
      completion(std::string("null"));
      return;
    }
    NSError* serialization_error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:value
                                                   options:NSJSONWritingFragmentsAllowed
                                                     error:&serialization_error];
    if (data == nil) {
      completion(PlatformError{
          "webview/javascript-result",
          ToString(serialization_error.localizedDescription),
          {},
      });
      return;
    }
    NSString* json = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    completion(ToString(json));
  }];
}

void ApplyProperties(
    const std::shared_ptr<IOSWebViewInstance>& instance,
    const WebViewProperties& properties,
    bool initial
) {
  const bool url_changed = initial || instance->properties.url != properties.url;
  instance->properties = properties;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  instance->view.configuration.preferences.javaScriptEnabled = properties.java_script_enabled;
#pragma clang diagnostic pop

  if (url_changed) {
    LoadUrl(instance, properties.url);
  }
}

bool RunCommand(
    const std::weak_ptr<IOSWebViewInstance>& weak_instance,
    WebViewControllerCommand command
) {
  std::shared_ptr<IOSWebViewInstance> instance = weak_instance.lock();
  if (!instance || instance->disposed || instance->view == nil) {
    return false;
  }

  if (const auto* value = std::get_if<WebViewCommand>(&command)) {
    switch (*value) {
    case WebViewCommand::Reload:
      [instance->view reload];
      return true;
    case WebViewCommand::StopLoading:
      [instance->view stopLoading];
      return true;
    case WebViewCommand::GoBack:
      if (!instance->view.canGoBack) {
        return false;
      }
      [instance->view goBack];
      return true;
    case WebViewCommand::GoForward:
      if (!instance->view.canGoForward) {
        return false;
      }
      [instance->view goForward];
      return true;
    }
  }
  if (const auto* value = std::get_if<WebViewLoadRequestCommand>(&command)) {
    LoadRequest(instance, value->request);
    return true;
  }
  if (const auto* value = std::get_if<WebViewLoadHtmlCommand>(&command)) {
    LoadHtml(instance, value->html);
    return true;
  }
  EvaluateJavaScript(
      instance,
      std::get<WebViewEvaluateJavaScriptCommand>(std::move(command))
  );
  return true;
}

std::shared_ptr<IOSWebViewInstance> CreateIOSWebView(
    const WebViewProperties& properties,
    PlatformEventEmitter events
) {
  WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  configuration.preferences.javaScriptEnabled = properties.java_script_enabled;
#pragma clang diagnostic pop

  auto instance = std::make_shared<IOSWebViewInstance>();
  instance->events = std::move(events);
  instance->navigation_delegate = [[HUXIOSWebViewNavigationDelegate alloc] initWithInstance:instance];
  [configuration.userContentController addScriptMessageHandler:instance->navigation_delegate
                                                            name:@"huxerui"];
  WKUserScript* bridge_script = [[WKUserScript alloc]
      initWithSource:@"Object.defineProperty(window, 'huxerui', { value: Object.freeze({ postMessage: function(value) { window.webkit.messageHandlers.huxerui.postMessage(String(value)); } }), configurable: false });"
      injectionTime:WKUserScriptInjectionTimeAtDocumentStart
      forMainFrameOnly:NO];
  [configuration.userContentController addUserScript:bridge_script];
  instance->view = [[WKWebView alloc] initWithFrame:CGRectZero configuration:configuration];
  instance->view.navigationDelegate = instance->navigation_delegate;
  [instance->view addObserver:instance->navigation_delegate
                   forKeyPath:@"estimatedProgress"
                      options:NSKeyValueObservingOptionNew
                      context:nullptr];
  instance->observing_progress = true;
  ApplyProperties(instance, properties, true);
  return instance;
}

void DisposeIOSWebView(IOSWebViewInstance& instance) {
  instance.disposed = true;
  [instance.view stopLoading];
  if (instance.observing_progress) {
    [instance.view removeObserver:instance.navigation_delegate forKeyPath:@"estimatedProgress"];
    instance.observing_progress = false;
  }
  instance.view.navigationDelegate = nil;
  [instance.view.configuration.userContentController removeScriptMessageHandlerForName:@"huxerui"];
  instance.navigation_delegate = nil;
  instance.view = nil;
}

} // namespace

void InstallPlatformWebView(RootContext& root) {
  ios::PlatformViewFactory<WebViewProperties, IOSWebViewInstance, WebViewController> factory{
      .create = [](UIViewController*, const WebViewProperties& properties, PlatformEventEmitter events) {
        return CreateIOSWebView(properties, std::move(events));
      },
      .view = [](const std::shared_ptr<IOSWebViewInstance>& instance) -> UIView* {
        return instance->view;
      },
      .update = [](IOSWebViewInstance& value, const WebViewProperties& properties) {
        ApplyProperties(value.shared_from_this(), properties, false);
      },
      .dispose = DisposeIOSWebView,
      .connect = [](IOSWebViewInstance& value, const WebViewController& controller) {
        std::weak_ptr<IOSWebViewInstance> weak_instance = value.shared_from_this();
        WebViewControllerAccess::Connect(
            controller,
            [weak_instance](WebViewControllerCommand command) {
              return RunCommand(weak_instance, std::move(command));
            }
        );
      },
      .disconnect = [](IOSWebViewInstance&, const WebViewController& controller) {
        WebViewControllerAccess::Disconnect(controller);
      },
  };

  root.RegisterPlatformView<WebViewProperties, WebViewController>(std::string(web_view_type), std::move(factory));
}

} // namespace huxerui::detail

@implementation HUXIOSWebViewNavigationDelegate

- (instancetype)initWithInstance:(std::weak_ptr<huxerui::detail::IOSWebViewInstance>)instance {
  self = [super init];
  if (self != nil) {
    instance_ = std::move(instance);
  }
  return self;
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)navigationAction
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
  std::shared_ptr<huxerui::detail::IOSWebViewInstance> instance = instance_.lock();
  if (!instance || instance->disposed) {
    decisionHandler(WKNavigationActionPolicyCancel);
    return;
  }

  NSString* url = navigationAction.request.URL.absoluteString;
  const bool is_main_frame = navigationAction.targetFrame == nil || navigationAction.targetFrame.mainFrame;
  const std::optional<bool> allowed =
      instance->events.Emit<huxerui::WebViewEvents::NavigationRequested>(huxerui::WebViewNavigationRequest{
          .url = huxerui::detail::ToString(url),
          .is_main_frame = is_main_frame,
      });
  decisionHandler(allowed.value_or(true) ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView*)webView didStartProvisionalNavigation:(WKNavigation*)navigation {
  static_cast<void>(navigation);
  std::shared_ptr<huxerui::detail::IOSWebViewInstance> instance = instance_.lock();
  if (instance) {
    instance->properties.url = huxerui::detail::ToString(webView.URL.absoluteString);
  }
  huxerui::detail::EmitStateEvent<huxerui::WebViewEvents::LoadStarted>(instance, true);
  huxerui::detail::EmitNavigationState(instance, true);
}

- (void)webView:(WKWebView*)webView didCommitNavigation:(WKNavigation*)navigation {
  static_cast<void>(webView);
  static_cast<void>(navigation);
  huxerui::detail::EmitNavigationState(instance_.lock(), true);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
  static_cast<void>(webView);
  static_cast<void>(navigation);
  std::shared_ptr<huxerui::detail::IOSWebViewInstance> instance = instance_.lock();
  huxerui::detail::EmitStateEvent<huxerui::WebViewEvents::LoadFinished>(instance, false);
  huxerui::detail::EmitNavigationState(instance, false);
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
  static_cast<void>(navigation);
  std::shared_ptr<huxerui::detail::IOSWebViewInstance> instance = instance_.lock();
  huxerui::detail::EmitLoadError(instance, error, webView.URL.absoluteString);
  huxerui::detail::EmitNavigationState(instance, false);
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
  static_cast<void>(navigation);
  std::shared_ptr<huxerui::detail::IOSWebViewInstance> instance = instance_.lock();
  huxerui::detail::EmitLoadError(instance, error, webView.URL.absoluteString);
  huxerui::detail::EmitNavigationState(instance, false);
}

- (void)userContentController:(WKUserContentController*)userContentController
      didReceiveScriptMessage:(WKScriptMessage*)message {
  static_cast<void>(userContentController);
  std::shared_ptr<huxerui::detail::IOSWebViewInstance> instance = instance_.lock();
  if (!instance || instance->disposed || ![message.name isEqualToString:@"huxerui"]) {
    return;
  }
  NSString* value = [message.body isKindOfClass:[NSString class]]
      ? static_cast<NSString*>(message.body)
      : [message.body description];
  instance->events.Emit<huxerui::WebViewEvents::MessageReceived>(huxerui::detail::ToString(value));
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context {
  static_cast<void>(object);
  static_cast<void>(change);
  static_cast<void>(context);
  if (![keyPath isEqualToString:@"estimatedProgress"]) {
    return;
  }
  std::shared_ptr<huxerui::detail::IOSWebViewInstance> instance = instance_.lock();
  huxerui::detail::EmitNavigationState(
      instance,
      instance && instance->view != nil ? instance->view.loading : false
  );
}

@end
