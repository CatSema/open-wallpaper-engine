module;

#include <cstdio>
#include <algorithm>
#include <cstring>

#if defined(__APPLE__)
#    include <mach-o/dyld.h>
#endif

module weweb;

import rstd.cppstd;
import :browser_host;
import :cef;
import :cef_internal;

using namespace rstd::literals;

namespace weweb
{

#if defined(__APPLE__)
namespace
{

std::filesystem::path CurrentExecutablePath() {
    std::uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) return {};

    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return std::filesystem::path(buffer.c_str());
}

void AddFrameworkCandidate(std::vector<std::filesystem::path>& candidates,
                           std::filesystem::path               path) {
    if (path.filename() == "Chromium Embedded Framework.framework") {
        path /= "Chromium Embedded Framework";
    }
    if (path.filename() != "Chromium Embedded Framework") return;

    std::error_code error;
    if (! std::filesystem::is_regular_file(path, error)) return;
    if (std::find(candidates.begin(), candidates.end(), path) == candidates.end()) {
        candidates.push_back(std::move(path));
    }
}

bool LoadCefFramework(bool helper, std::filesystem::path* loaded_binary) {
    std::vector<std::filesystem::path> candidates;
    if (const char* override_path = std::getenv("OWE_CEF_FRAMEWORK_PATH");
        override_path != nullptr && override_path[0] != '\0') {
        AddFrameworkCandidate(candidates, override_path);
    }

    const auto executable = CurrentExecutablePath();
    if (! executable.empty()) {
        const auto executable_dir = executable.parent_path();
        const auto main_root      = executable_dir / "../Frameworks";
        const auto helper_root    = executable_dir / "../../..";
        if (helper) {
            AddFrameworkCandidate(
                candidates,
                helper_root / "Chromium Embedded Framework.framework/Chromium Embedded Framework");
        }
        // A single executable can also be used as the browser subprocess
        // entry point. In that layout the helper process still loads from the
        // main app's Contents/Frameworks directory.
        AddFrameworkCandidate(
            candidates,
            main_root / "Chromium Embedded Framework.framework/Chromium Embedded Framework");

        // Development builds may not be wrapped in an application bundle yet.
        AddFrameworkCandidate(
            candidates,
            executable_dir / "Chromium Embedded Framework.framework/Chromium Embedded Framework");
        AddFrameworkCandidate(
            candidates,
            executable_dir /
                "../Chromium Embedded Framework.framework/Chromium Embedded Framework");
        AddFrameworkCandidate(
            candidates,
            executable_dir /
                "../Frameworks/Chromium Embedded Framework.framework/Chromium Embedded Framework");
    }

    for (const auto& candidate : candidates) {
        if (cef_load_library(candidate.string().c_str())) {
            if (loaded_binary != nullptr) *loaded_binary = candidate;
            if (! helper) {
                std::fprintf(stderr, "weweb: loaded CEF framework from %s\n", candidate.c_str());
            }
            return true;
        }
    }

    std::fprintf(stderr,
                 "weweb: unable to load Chromium Embedded Framework; "
                 "set OWE_CEF_FRAMEWORK_PATH to the framework binary\n");
    return false;
}

bool IsCefHelperProcess(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::strncmp(argv[i], "--type=", 7) == 0) return true;
    }
    return false;
}

} // namespace
#endif

struct BrowserHost::Impl {
    CefRefPtr<AppHandler>       app;
    CefRefPtr<OsrRenderHandler> osr;
    CefRefPtr<ClientHandler>    client;
    AcceleratedPaintCallback    accel_cb;
    CpuPaintCallback            cpu_cb;
    std::function<void(bool)>   audio_demand_cb;
    std::atomic<bool>           should_exit { false };
    bool                        initialised { false };
    // Stash the original argv from RunOrExitIfHelper; CefInitialize needs
    // the real argv to derive the per-child --type=… / --icu-data-file=…
    // switches it forwards to subprocesses.
    int    saved_argc { 0 };
    char** saved_argv { nullptr };
#if defined(__APPLE__)
    bool                  cef_loaded { false };
    std::filesystem::path cef_framework_binary;
#endif
};

BrowserHost::BrowserHost(): impl_(std::make_unique<Impl>()) { impl_->app = new AppHandler(); }

BrowserHost::~BrowserHost() { Shutdown(); }

int BrowserHost::RunOrExitIfHelper(int argc, char** argv) {
    impl_->saved_argc = argc;
    impl_->saved_argv = argv;
#if defined(__APPLE__)
    if (! impl_->cef_loaded) {
        if (! LoadCefFramework(IsCefHelperProcess(argc, argv), &impl_->cef_framework_binary)) {
            return 1;
        }
        impl_->cef_loaded = true;
    }
#endif
    CefMainArgs main_args(argc, argv);
    const int   result = CefExecuteProcess(main_args, impl_->app.get(), nullptr);
#if defined(__APPLE__)
    if (result >= 0) {
        cef_unload_library();
        impl_->cef_loaded = false;
    }
#endif
    return result;
}

bool BrowserHost::Init(const InitOptions& opts) {
    if (impl_->initialised) {
        std::fprintf(stderr, "weweb: BrowserHost::Init called twice\n");
        return false;
    }

#if defined(__APPLE__)
    if (! impl_->cef_loaded) {
        if (! LoadCefFramework(false, &impl_->cef_framework_binary)) return false;
        impl_->cef_loaded = true;
    }
#endif

    CefMainArgs main_args(impl_->saved_argc, impl_->saved_argv);

    CefSettings settings;
    settings.no_sandbox                   = true;
    settings.windowless_rendering_enabled = true; // OSR mode
    settings.multi_threaded_message_loop  = false;
    settings.log_severity                 = LOGSEVERITY_WARNING;
    // WW_CEF_DEBUG=1 ⇒ flip CEF's own log threshold so the chromium VLOG
    // stream actually fires (--enable-logging=stderr alone is gated by
    // settings.log_severity). WW_CEF_LOG_FILE redirects the file sink.
    if (const char* dbg = std::getenv("WW_CEF_DEBUG"); dbg && dbg[0] && dbg[0] != '0') {
        settings.log_severity = LOGSEVERITY_VERBOSE;
    }
    if (const char* lf = std::getenv("WW_CEF_LOG_FILE"); lf && lf[0]) {
        CefString(&settings.log_file) = lf;
    }

    auto set_cef_path = [](cef_string_t* dest, const std::filesystem::path& p) {
        if (p.empty()) return;
        CefString cef_str { dest };
        cef_str = p.string();
    };
#if defined(__APPLE__)
    // A single executable is used for the browser and CEF subprocesses in
    // development builds. The same entry point calls CefExecuteProcess
    // before entering the browser loop, so no helper app is required.
    set_cef_path(&settings.browser_subprocess_path, CurrentExecutablePath());
    if (const char* override_path = std::getenv("OWE_CEF_FRAMEWORK_PATH");
        override_path != nullptr && override_path[0] != '\0') {
        std::filesystem::path framework_path(override_path);
        if (framework_path.filename() == "Chromium Embedded Framework") {
            framework_path = framework_path.parent_path();
        }
        set_cef_path(&settings.framework_dir_path, framework_path);
    }
    if (settings.framework_dir_path.length == 0 && ! impl_->cef_framework_binary.empty()) {
        set_cef_path(&settings.framework_dir_path, impl_->cef_framework_binary.parent_path());
    }
#endif
    set_cef_path(&settings.resources_dir_path, opts.resources_dir);
    set_cef_path(&settings.locales_dir_path, opts.locales_dir);
    set_cef_path(&settings.root_cache_path, opts.cache_dir);

    if (opts.enable_remote_debugging && opts.remote_debugging_port > 0) {
        settings.remote_debugging_port = opts.remote_debugging_port;
    }

    // Stash before CefInitialize; AppHandler::OnBeforeCommandLineProcessing
    // runs synchronously inside it and reads the flag.
    impl_->app->SetMuteAudio(! opts.enable_audio);
    impl_->app->SetSharedTextureEnabled(opts.shared_texture_enabled);
    impl_->app->SetRenderNodeOverride(opts.render_node_override);

    if (! CefInitialize(main_args, settings, impl_->app.get(), nullptr)) {
        std::fprintf(stderr, "weweb: CefInitialize failed\n");
#if defined(__APPLE__)
        cef_unload_library();
        impl_->cef_loaded = false;
#endif
        return false;
    }
    impl_->initialised = true;
    return true;
}

bool BrowserHost::OpenWallpaper(const WebManifest&           manifest,
                                const std::filesystem::path& workshop_dir, int width, int height) {
    return OpenWallpaper(manifest, workshop_dir, width, height, OpenOptions {});
}

bool BrowserHost::OpenWallpaper(const WebManifest&           manifest,
                                const std::filesystem::path& workshop_dir, int width, int height,
                                OpenOptions opts) {
    if (! impl_->initialised) {
        std::fprintf(stderr, "weweb: OpenWallpaper before Init\n");
        return false;
    }

    impl_->osr = new OsrRenderHandler();
    impl_->osr->SetViewSize(width, height);
    if (impl_->accel_cb) {
        impl_->osr->SetAcceleratedPaintCallback(impl_->accel_cb);
    }
    if (impl_->cpu_cb) {
        impl_->osr->SetCpuPaintCallback(impl_->cpu_cb);
    }
    impl_->osr->SetDeviceScaleFactor(opts.device_scale_factor);

    impl_->client =
        new ClientHandler(manifest.user_props.clone(), impl_->osr, opts.initially_muted);
    impl_->client->SetAudioDemandCallback(impl_->audio_demand_cb);
    impl_->client->SetCloseCallback([this] {
        impl_->should_exit.store(true);
    });

    auto        entry = workshop_dir / manifest.entry_html;
    std::string url   = "file://" + entry.string();

    CefWindowInfo info;
    info.SetAsWindowless(0); // no parent window — pure OSR
    info.shared_texture_enabled = opts.shared_texture_enabled ? 1 : 0;

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = opts.frame_rate > 0 ? opts.frame_rate : 60;

    const bool created = CefBrowserHost::CreateBrowser(
        info, impl_->client.get(), url, browser_settings, nullptr, nullptr);
    if (! created) {
        std::fprintf(stderr, "weweb: CefBrowserHost::CreateBrowser failed for %s\n", url.c_str());
    }
    return created;
}

void BrowserHost::SetAcceleratedPaintCallback(AcceleratedPaintCallback cb) {
    impl_->accel_cb = std::move(cb);
    if (impl_->osr) impl_->osr->SetAcceleratedPaintCallback(impl_->accel_cb);
}

void BrowserHost::SetCpuPaintCallback(CpuPaintCallback cb) {
    impl_->cpu_cb = std::move(cb);
    if (impl_->osr) impl_->osr->SetCpuPaintCallback(impl_->cpu_cb);
}

void BrowserHost::SetAudioResponseDemandCallback(std::function<void(bool)> cb) {
    impl_->audio_demand_cb = std::move(cb);
    if (impl_->client) impl_->client->SetAudioDemandCallback(impl_->audio_demand_cb);
}

void BrowserHost::Invalidate() {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->Invalidate(PET_VIEW);
}

void BrowserHost::OnResize(int width, int height) { OnResize(width, height, 1.0f); }

void BrowserHost::OnResize(int width, int height, float device_scale_factor) {
    if (width <= 0 || height <= 0) return;
    if (! impl_->osr) return;
    impl_->osr->SetViewSize(width, height);
    impl_->osr->SetDeviceScaleFactor(device_scale_factor);
    if (! impl_->client) return;
    if (auto b = impl_->client->GetBrowser(); b && b->GetHost()) {
        b->GetHost()->WasResized();
    }
}

void BrowserHost::OnMouseMove(int x, int y, bool left_down) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x         = x;
    ev.y         = y;
    ev.modifiers = left_down ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    b->GetHost()->SendMouseMoveEvent(ev, /*mouseLeave=*/false);
}

void BrowserHost::OnMouseButton(int x, int y, int cef_button, bool down, int click_count) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    b->GetHost()->SendMouseClickEvent(ev,
                                      static_cast<cef_mouse_button_type_t>(cef_button),
                                      /*mouseUp=*/! down,
                                      click_count);
}

void BrowserHost::OnMouseWheel(int x, int y, int delta_x, int delta_y) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    b->GetHost()->SendMouseWheelEvent(ev, delta_x, delta_y);
}

void BrowserHost::OnKey(int cef_key_event_type, int native_key_code, int windows_key_code,
                        int modifiers, unsigned int unicode_char) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefKeyEvent ev;
    ev.type                 = static_cast<cef_key_event_type_t>(cef_key_event_type);
    ev.native_key_code      = native_key_code;
    ev.windows_key_code     = windows_key_code;
    ev.modifiers            = modifiers;
    ev.character            = static_cast<char16_t>(unicode_char);
    ev.unmodified_character = ev.character;
    b->GetHost()->SendKeyEvent(ev);
}

void BrowserHost::OnFocus(bool gained) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    b->GetHost()->SetFocus(gained);
}

void BrowserHost::Pump() {
    if (impl_->initialised) CefDoMessageLoopWork();
}

void BrowserHost::ApplyVolume(float volume) {
    if (impl_->client) impl_->client->SetAudioMuted(volume <= 0.0f);
    auto object = rstd::json::Map::make();
    object.insert(::alloc::string::String::make("value"_str),
                  rstd::into<owe::Json>(rstd::f32(volume)));
    auto v = owe::Json::Object(rstd::move(object));
    ApplyUserProperty("audio", v);
}

void BrowserHost::SetFrameRate(int fps) {
    if (fps <= 0 || ! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->SetWindowlessFrameRate(fps);
}

void BrowserHost::SetPaused(bool paused) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->WasHidden(paused);
}

void BrowserHost::ApplyUserProperty(std::string_view key, const owe::Json& value) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b) return;
    auto frame = b->GetMainFrame();
    if (! frame) return;

    // Page-side listener convention (mirrors BuildPropertyListenerSnippet):
    //   window.wallpaperPropertyListener.applyUserProperties({key: {value: V}}).
    auto object = rstd::json::Map::make();
    object.insert(::alloc::string::String::make(rstd::cppstd::as_str(key).unwrap()), value.clone());
    auto        properties = owe::Json::Object(rstd::move(object));
    std::string snippet =
        "(function(){"
        "  if (typeof window.wallpaperPropertyListener !== 'object') return;"
        "  if (typeof window.wallpaperPropertyListener.applyUserProperties !== 'function') return;"
        "  try {"
        "    window.wallpaperPropertyListener.applyUserProperties(";
    snippet += owe::Dump(properties);
    snippet += ");"
               "  } catch (e) {"
               "    console.error('weweb: applyUserProperties patch threw:', e);"
               "  }"
               "})();";
    frame->ExecuteJavaScript(snippet, "weweb://internal/apply_user_property.js", 0);
}

void BrowserHost::PushAudioData(const float* data, std::size_t count) {
    if (! impl_->client || ! data || count == 0) return;
    auto b = impl_->client->GetBrowser();
    if (! b) return;
    auto frame = b->GetMainFrame();
    if (! frame) return;

    std::string snippet;
    snippet.reserve(count * 8 + 64);
    snippet += "(function(){if(!window.__weweb_pushAudio)return;window.__weweb_pushAudio([";
    char buf[32];
    for (std::size_t i = 0; i < count; ++i) {
        if (i) snippet += ',';
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(data[i]));
        snippet += buf;
    }
    snippet += "]);})();";
    frame->ExecuteJavaScript(snippet, "weweb://internal/push_audio.js", 0);
}

bool BrowserHost::ShouldExit() const { return impl_->should_exit.load(); }

void BrowserHost::RequestClose() { impl_->should_exit.store(true); }

void BrowserHost::Shutdown() {
    if (impl_->initialised) {
        CefShutdown();
        impl_->initialised = false;
    }
#if defined(__APPLE__)
    if (impl_->cef_loaded) {
        cef_unload_library();
        impl_->cef_loaded = false;
    }
#endif
}

} // namespace weweb
