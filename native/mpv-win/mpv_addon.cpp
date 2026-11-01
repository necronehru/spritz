#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <napi.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_d3d11.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <cstdio>
#include <memory>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

static mpv_handle* gMpv = nullptr;
static mpv_render_context* gRender = nullptr;
static ComPtr<ID3D11Device> gDevice;
static ComPtr<ID3D11DeviceContext> gContext;
static ComPtr<IDXGISwapChain1> gSwapChain;
static ComPtr<IDCompositionDevice> gComposition;
static ComPtr<IDCompositionTarget> gTarget;
static ComPtr<IDCompositionVisual> gVisual;
static HWND gHwnd = nullptr;

static std::atomic<bool> gRunning{false};
static std::atomic<bool> gStarted{false};
static std::thread gRenderThread;
static std::thread gEventThread;
static std::mutex gListenerMutex;
static Napi::ThreadSafeFunction gListener;
static bool gHasListener = false;

static std::atomic<unsigned long long> gFrames{0};
static std::atomic<int> gLastRenderRc{0};
static std::atomic<bool> gRenderReady{false};
static std::atomic<bool> gRenderInitDone{false};


static void jsonEscape(const char* value, std::string& out) {
    if (!value) return;
    for (const unsigned char c : std::string(value)) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", (unsigned int)c);
                out += b;
            } else out += (char)c;
        }
    }
}

static void nodeToJson(const mpv_node* n, std::string& out) {
    if (!n) { out += "null"; return; }
    switch (n->format) {
    case MPV_FORMAT_STRING:
        out += '"'; jsonEscape(n->u.string, out); out += '"'; break;
    case MPV_FORMAT_FLAG:
        out += n->u.flag ? "true" : "false"; break;
    case MPV_FORMAT_INT64:
        out += std::to_string(n->u.int64); break;
    case MPV_FORMAT_DOUBLE:
        out += std::to_string(n->u.double_); break;
    case MPV_FORMAT_NODE_ARRAY:
    case MPV_FORMAT_NODE_MAP: {
        const mpv_node_list* list = n->u.list;
        const bool map = n->format == MPV_FORMAT_NODE_MAP;
        if (!list) { out += map ? "{}" : "[]"; break; }
        out += map ? '{' : '[';
        for (int i = 0; i < list->num; ++i) {
            if (i) out += ',';
            if (map) {
                out += '"';
                jsonEscape(list->keys ? list->keys[i] : nullptr, out);
                out += "\":";
            }
            nodeToJson(&list->values[i], out);
        }
        out += map ? '}' : ']';
        break;
    }
    default:
        out += "null"; break;
    }
}

static void EmitEvent(const mpv_event* event) {
    struct Payload {
        std::string type, name, stringValue;
        double numberValue = 0;
        bool boolValue = false;
        int valueKind = 0;
        int reason = 0;
        bool hasReason = false;
    };

    auto* p = new Payload();

    switch (event->event_id) {
    case MPV_EVENT_SHUTDOWN: p->type = "shutdown"; break;
    case MPV_EVENT_START_FILE: p->type = "start-file"; break;
    case MPV_EVENT_FILE_LOADED: p->type = "file-loaded"; break;

    case MPV_EVENT_END_FILE: {
        p->type = "end-file";
        auto* e = static_cast<mpv_event_end_file*>(event->data);
        if (e) { p->reason = (int)e->reason; p->hasReason = true; }
        break;
    }

    case MPV_EVENT_LOG_MESSAGE: {
        auto* e = static_cast<mpv_event_log_message*>(event->data);
        if (!e || !e->text) { delete p; return; }
        p->type = "log";
        p->name = e->prefix ? e->prefix : "";
        p->stringValue = e->text;
        p->valueKind = 3;
        break;
    }

    case MPV_EVENT_PROPERTY_CHANGE: {
        auto* e = static_cast<mpv_event_property*>(event->data);
        if (!e || !e->name) { delete p; return; }
        p->type = "property-change";
        p->name = e->name;

        switch (e->format) {
        case MPV_FORMAT_DOUBLE:
            if (e->data) {
                p->numberValue = *static_cast<double*>(e->data);
                p->valueKind = 1;
            }
            break;
        case MPV_FORMAT_INT64:
            if (e->data) {
                p->numberValue = (double)*static_cast<int64_t*>(e->data);
                p->valueKind = 1;
            }
            break;
        case MPV_FORMAT_FLAG:
            if (e->data) {
                p->boolValue = *static_cast<int*>(e->data) != 0;
                p->valueKind = 2;
            }
            break;
        case MPV_FORMAT_STRING:
        case MPV_FORMAT_OSD_STRING:
            if (e->data) {
                char** v = static_cast<char**>(e->data);
                p->stringValue = (v && *v) ? *v : "";
                p->valueKind = 3;
            }
            break;
        case MPV_FORMAT_NODE:
            if (e->data) {
                nodeToJson(static_cast<mpv_node*>(e->data), p->stringValue);
                p->valueKind = 3;
            }
            break;
        default:
            break;
        }
        break;
    }

    default:
        delete p;
        return;
    }

    std::lock_guard<std::mutex> lock(gListenerMutex);
    if (!gHasListener) { delete p; return; }

    napi_status status = gListener.NonBlockingCall(
        p, [](Napi::Env env, Napi::Function cb, Payload* data) {
            std::unique_ptr<Payload> owned(data);
            if (env == nullptr || cb.IsEmpty()) return;

            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", data->type);
            if (!data->name.empty()) obj.Set("name", data->name);

            switch (data->valueKind) {
            case 1: obj.Set("value", data->numberValue); break;
            case 2: obj.Set("value", data->boolValue); break;
            case 3: obj.Set("value", data->stringValue); break;
            default:
                if (data->type == "property-change")
                    obj.Set("value", env.Null());
                break;
            }

            if (data->hasReason) obj.Set("reason", data->reason);
            try { cb.Call({obj}); }
            catch (const Napi::Error&) {}
        });

    if (status != napi_ok) delete p;
}

static void EventLoop() {
    while (gRunning && gMpv) {
        const mpv_event* event = mpv_wait_event(gMpv, 0.1);
        if (!event || event->event_id == MPV_EVENT_NONE) continue;
        if (event->event_id == MPV_EVENT_SHUTDOWN) break;
        EmitEvent(event);
    }
}

static void RenderLoop() {
    gRenderReady = false;
    if (!gMpv || !gDevice || !gSwapChain) {
        gLastRenderRc = MPV_ERROR_UNINITIALIZED;
        gRenderInitDone = true;
        return;
    }

    mpv_d3d11_init_params initParams{};
    initParams.device = gDevice.Get();

    mpv_render_param createParams[] = {
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_D3D11) },
        { MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &initParams },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };

    int rc = mpv_render_context_create(&gRender, gMpv, createParams);
    if (rc < 0) {
        gLastRenderRc = rc;
        gRenderReady = false;
        gRenderInitDone = true;
        return;
    }

    gRenderReady = true;
    gRenderInitDone = true;
    while (gRunning) {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = gSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (SUCCEEDED(hr)) {
            mpv_d3d11_fbo fbo{};
            fbo.tex = backBuffer.Get();

            mpv_render_param params[] = {
                { MPV_RENDER_PARAM_D3D11_FBO, &fbo },
                { MPV_RENDER_PARAM_INVALID, nullptr }
            };

            rc = mpv_render_context_render(gRender, params);
            gLastRenderRc = rc;

            if (rc >= 0) {
                hr = gSwapChain->Present(1, 0);
                if (SUCCEEDED(hr)) ++gFrames;
            }
        }
        Sleep(8);
    }

    gRenderReady = false;
    if (gRender) {
        mpv_render_context_free(gRender);
        gRender = nullptr;
    }
}

static void Cleanup() {
    gRunning = false;

    if (gEventThread.joinable()) gEventThread.join();
    if (gRenderThread.joinable()) gRenderThread.join();

    if (gMpv) {
        mpv_terminate_destroy(gMpv);
        gMpv = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(gListenerMutex);
        if (gHasListener) {
            gListener.Release();
            gHasListener = false;
        }
    }

    if (gComposition) gComposition->Commit();

    gVisual.Reset();
    gTarget.Reset();
    gComposition.Reset();
    gSwapChain.Reset();
    gContext.Reset();
    gDevice.Reset();
    gHwnd = nullptr;
    gRenderReady = false;
    gRenderInitDone = false;
    gStarted = false;
}

static Napi::Value ApiVersion(const Napi::CallbackInfo& info) {
    return Napi::Number::New(info.Env(), 1);
}

static Napi::Value Probe(const Napi::CallbackInfo& info) {
    Napi::Object out = Napi::Object::New(info.Env());
    out.Set("mpv", true);
    out.Set("renderer", "d3d11");
    out.Set("platform", "win32");
    return out;
}

static Napi::Value AttachTestSurface(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsBuffer()) {
        Napi::TypeError::New(env, "Expected Electron native window handle Buffer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    auto buffer = info[0].As<Napi::Buffer<char>>();
    if (buffer.Length() < sizeof(HWND)) {
        Napi::TypeError::New(env, "Invalid native window handle").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Cleanup();
    memcpy(&gHwnd, buffer.Data(), sizeof(HWND));

    if (!gHwnd || !IsWindow(gHwnd)) {
        Napi::Error::New(env, "Invalid Electron HWND").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    RECT rect{};
    GetClientRect(gHwnd, &rect);
    UINT width = static_cast<UINT>(rect.right - rect.left);
    UINT height = static_cast<UINT>(rect.bottom - rect.top);
    if (!width) width = 1280;
    if (!height) height = 720;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &gDevice, nullptr, &gContext
    );

    if (FAILED(hr)) {
        Napi::Error::New(env, "D3D11CreateDevice failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = gDevice.As(&dxgiDevice);
    if (FAILED(hr)) {
        Napi::Error::New(env, "Query IDXGIDevice failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    ComPtr<IDXGIFactory2> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Napi::Error::New(env, "CreateDXGIFactory1 failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    hr = factory->CreateSwapChainForComposition(gDevice.Get(), &desc, nullptr, &gSwapChain);
    if (FAILED(hr)) {
        Napi::Error::New(env, "CreateSwapChainForComposition failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&gComposition));
    if (FAILED(hr)) {
        Napi::Error::New(env, "DCompositionCreateDevice failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    hr = gComposition->CreateTargetForHwnd(gHwnd, TRUE, &gTarget);
    if (FAILED(hr)) {
        Napi::Error::New(env, "CreateTargetForHwnd failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    hr = gComposition->CreateVisual(&gVisual);
    if (FAILED(hr)) {
        Napi::Error::New(env, "CreateVisual failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    hr = gVisual->SetContent(gSwapChain.Get());
    if (FAILED(hr)) {
        Napi::Error::New(env, "DirectComposition SetContent failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    hr = gTarget->SetRoot(gVisual.Get());
    if (FAILED(hr) || FAILED(gComposition->Commit())) {
        Napi::Error::New(env, "DirectComposition commit failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    return Napi::Boolean::New(env, true);
}

static Napi::Value SetEventListener(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (gStarted) {
        Napi::Error::New(env, "Register event listener before startPlayer").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected event listener function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::lock_guard<std::mutex> lock(gListenerMutex);
    if (gHasListener) gListener.Release();

    gListener = Napi::ThreadSafeFunction::New(
        env, info[0].As<Napi::Function>(), "mpv-events", 256, 1
    );
    gHasListener = true;
    return env.Undefined();
}

static Napi::Value StartPlayer(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!gHwnd || !gDevice || !gSwapChain) {
        Napi::Error::New(env, "Call attachTestSurface before startPlayer").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (gStarted) return Napi::Boolean::New(env, true);

    gMpv = mpv_create();
    if (!gMpv) {
        Napi::Error::New(env, "mpv_create failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    mpv_set_option_string(gMpv, "terminal", "no");
    mpv_set_option_string(gMpv, "msg-level", "all=error");
    mpv_set_option_string(gMpv, "vo", "libmpv");
    mpv_set_option_string(gMpv, "ao", "wasapi");
    mpv_set_option_string(gMpv, "hwdec", "auto");
    mpv_set_option_string(gMpv, "profile", "high-quality");
    mpv_set_option_string(gMpv, "target-colorspace-hint", "yes");
    mpv_set_option_string(gMpv, "tone-mapping", "bt.2390");
    mpv_set_option_string(gMpv, "gamut-mapping-mode", "perceptual");

    int rc = mpv_initialize(gMpv);
    if (rc < 0) {
        mpv_terminate_destroy(gMpv);
        gMpv = nullptr;
        Napi::Error::New(env, "mpv_initialize failed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    mpv_observe_property(gMpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(gMpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(gMpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(gMpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(gMpv, 0, "seekable", MPV_FORMAT_FLAG);
    mpv_observe_property(gMpv, 0, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(gMpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(gMpv, 0, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(gMpv, 0, "dheight", MPV_FORMAT_INT64);
    mpv_observe_property(gMpv, 0, "hwdec-current", MPV_FORMAT_STRING);
    mpv_observe_property(gMpv, 0, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(gMpv, 0, "aid", MPV_FORMAT_STRING);
    mpv_observe_property(gMpv, 0, "sid", MPV_FORMAT_STRING);

    mpv_request_log_messages(gMpv, "error");

    gRenderReady = false;
    gRenderInitDone = false;
    gLastRenderRc = 0;
    gRunning = true;
    gStarted = true;

    gRenderThread = std::thread(RenderLoop);
    gEventThread = std::thread(EventLoop);

    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!gRenderInitDone.load() && GetTickCount64() < deadline) {
        Sleep(1);
    }

    if (!gRenderReady.load()) {
        const int renderRc = gLastRenderRc.load();
        Cleanup();
        Napi::Error::New(
            env,
            "D3D11 render initialization failed (mpv error " +
                std::to_string(renderRc) + ")"
        ).ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Object result = Napi::Object::New(env);
    result.Set("initialized", true);
    result.Set("renderer", "d3d11");
    result.Set("renderActive", true);
    return result;
}

static Napi::Value LoadFile(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!gMpv || info.Length() < 1) return Napi::Number::New(env, -1);

    std::string url = info[0].ToString().Utf8Value();
    const char* args[] = { "loadfile", url.c_str(), "replace", nullptr };
    int rc = mpv_command(gMpv, args);

    Napi::Object out = Napi::Object::New(env);
    out.Set("loadRc", rc);
    return out;
}

static Napi::Value Command(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!gMpv || info.Length() < 1) return Napi::Number::New(env, -1);

    std::vector<std::string> values;
    std::vector<const char*> args;

    for (size_t i = 0; i < info.Length(); ++i)
        values.push_back(info[i].ToString().Utf8Value());

    for (auto& value : values) args.push_back(value.c_str());
    args.push_back(nullptr);

    return Napi::Number::New(env, mpv_command(gMpv, args.data()));
}

static Napi::Value SetProperty(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!gMpv || info.Length() < 2) return Napi::Boolean::New(env, false);

    std::string name = info[0].ToString().Utf8Value();
    std::string value = info[1].ToString().Utf8Value();
    int rc = mpv_set_property_string(gMpv, name.c_str(), value.c_str());

    return Napi::Boolean::New(env, rc >= 0);
}

static Napi::Value PlayerStat(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object out = Napi::Object::New(env);

    if (!gMpv) return out;

    double time = 0, duration = 0;
    int64_t width = 0, height = 0;
    int pause = 0;
    char* aid = nullptr;

    mpv_get_property(gMpv, "time-pos", MPV_FORMAT_DOUBLE, &time);
    mpv_get_property(gMpv, "duration", MPV_FORMAT_DOUBLE, &duration);
    mpv_get_property(gMpv, "dwidth", MPV_FORMAT_INT64, &width);
    mpv_get_property(gMpv, "dheight", MPV_FORMAT_INT64, &height);
    mpv_get_property(gMpv, "pause", MPV_FORMAT_FLAG, &pause);
    if (mpv_get_property(gMpv, "aid", MPV_FORMAT_STRING, &aid) >= 0 && aid)
        out.Set("aid", aid);

    if (aid) mpv_free(aid);

    out.Set("timePos", time);
    out.Set("duration", duration);
    out.Set("width", static_cast<double>(width));
    out.Set("height", static_cast<double>(height));
    out.Set("paused", pause != 0);
    out.Set("renderActive", gRenderReady.load());
    out.Set("frameCount", static_cast<double>(gFrames.load()));
    out.Set("lastRenderRc", gLastRenderRc.load());
    return out;
}

static Napi::Value MediaStats(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object out = Napi::Object::New(env);
    if (!gMpv) return out;

    char* video = nullptr;
    char* audio = nullptr;
    char* hwdec = nullptr;

    if (mpv_get_property(gMpv, "video-codec", MPV_FORMAT_STRING, &video) >= 0 && video)
        out.Set("vcodec", video);
    if (mpv_get_property(gMpv, "audio-codec-name", MPV_FORMAT_STRING, &audio) >= 0 && audio)
        out.Set("acodec", audio);
    if (mpv_get_property(gMpv, "hwdec-current", MPV_FORMAT_STRING, &hwdec) >= 0 && hwdec)
        out.Set("hwdec", hwdec);

    if (video) mpv_free(video);
    if (audio) mpv_free(audio);
    if (hwdec) mpv_free(hwdec);

    return out;
}

static Napi::Value GlSelfCheck(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object out = Napi::Object::New(env);
    out.Set("renderer", "d3d11");
    out.Set("frameCount", static_cast<double>(gFrames.load()));
    out.Set("lastRenderRc", gLastRenderRc.load());
    out.Set("renderActive", gRenderReady.load());
    out.Set("compositionAttached", gTarget != nullptr);
    return out;
}

static Napi::Value Detach(const Napi::CallbackInfo& info) {
    Cleanup();
    return info.Env().Undefined();
}

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("apiVersion", Napi::Function::New(env, ApiVersion));
    exports.Set("probe", Napi::Function::New(env, Probe));
    exports.Set("attachTestSurface", Napi::Function::New(env, AttachTestSurface));
    exports.Set("setEventListener", Napi::Function::New(env, SetEventListener));
    exports.Set("startPlayer", Napi::Function::New(env, StartPlayer));
    exports.Set("loadFile", Napi::Function::New(env, LoadFile));
    exports.Set("command", Napi::Function::New(env, Command));
    exports.Set("playerStat", Napi::Function::New(env, PlayerStat));
    exports.Set("mediaStats", Napi::Function::New(env, MediaStats));
    exports.Set("setProperty", Napi::Function::New(env, SetProperty));
    exports.Set("glSelfCheck", Napi::Function::New(env, GlSelfCheck));
    exports.Set("detach", Napi::Function::New(env, Detach));

    napi_add_env_cleanup_hook(env, [](void*) { Cleanup(); }, nullptr);
    return exports;
}

NODE_API_MODULE(mpv_render, Init)
