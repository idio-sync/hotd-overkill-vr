// fakevr: IVRCompositor, IVROverlay, the per-second log line and captures.
//
// WaitGetPoses paces the caller at the headset's rate (it returns at the next vsync of a 120 Hz
// clock) and hands out the scripted poses. Submit checks what SteamVR checks (a texture, a known
// D3D11 format with shader-resource binding, no multisampling, bounds inside 0..1, one submit per
// eye per WaitGetPoses) and returns the matching EVRCompositorError. Overlays are recorded
// (texture, width, flags, absolute transform, visibility, sort order).
//
// Captures (`capture NAME`), written to hovr\fakevr\ at the next WaitGetPoses for the frame just
// submitted:
//   NAME_submit0/1.png  each eye's submitted texture region (Submit bounds), as submitted
//   NAME_ov_<key>.png   each visible overlay's texture (with alpha)
//   NAME_view0/1.png    the "headset view": the eye image plus every visible overlay drawn the way
//                       SteamVR places it - the overlay quad in tracking space (absolute
//                       transform, width, height from the texture's aspect), seen from that eye's
//                       real pose (head pose of the frame x eye-to-head) through that eye's
//                       projection; premultiplied / ignore-alpha flags and overlay alpha honoured;
//                       overlays are drawn over the scene (SteamVR has no depth for them), in sort
//                       order, double-sided
//   NAME_truth0/1.png   the same plus ground truth from the fake's own math: each controller's
//                       pointing ray (right cyan, left magenta) with dots at 0.25/0.5/1/2/5/10 m and
//                       a cross at the tip, the true horizon (yellow, eye height, level), overlay
//                       outlines (green)
//   NAME.txt            poses, eye matrices, tangents, overlays, submit bounds, haptics, stats
// The view is not lens-distorted and not reprojected: it is the image a perfect display at the
// eye's projection would show for the pose the frame was rendered with.
#include "fakevr.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fv {

const char* compositor_error_name(int e) {
    switch (e) {
        case 0: return "None";
        case 1: return "RequestFailed";
        case 100: return "IncompatibleVersion";
        case 101: return "DoNotHaveFocus";
        case 102: return "InvalidTexture";
        case 103: return "IsNotSceneApplication";
        case 104: return "TextureIsOnWrongDevice";
        case 105: return "TextureUsesUnsupportedFormat";
        case 106: return "SharedTexturesNotSupported";
        case 107: return "IndexOutOfRange";
        case 108: return "AlreadySubmitted";
        case 109: return "InvalidBounds";
        case 110: return "AlreadySet";
        default: return "?";
    }
}

const char* overlay_error_name(int e) {
    switch (e) {
        case 0: return "None";
        case 10: return "UnknownOverlay";
        case 11: return "InvalidHandle";
        case 15: return "KeyTooLong";
        case 16: return "NameTooLong";
        case 17: return "KeyInUse";
        case 20: return "InvalidParameter";
        case 24: return "InvalidTexture";
        default: return "?";
    }
}

static void note_error(const char* what, int e, const char* name) {
    State& s = st();
    ++s.stat_errors, ++s.total_errors;
    char b[160];
    snprintf(b, sizeof b, "%s -> %d %s", what, e, name);
    s.last_error = b;
    if (s.total_errors <= 50) logf("ERROR returned to the app: %s", b);
}

static bool format_ok(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return true;
        default:
            return false;
    }
}

// Checks a D3D11 texture as SteamVR would; 0 or the compositor error.
static int check_texture(const vr::Texture_t* t, D3D11_TEXTURE2D_DESC* desc) {
    if (!t || !t->handle) return vr::VRCompositorError_InvalidTexture;
    if (t->eType != vr::TextureType_DirectX) return vr::VRCompositorError_InvalidTexture;
    auto* tex = static_cast<ID3D11Texture2D*>(t->handle);
    ID3D11Texture2D* probe = nullptr;
    if (FAILED(tex->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&probe))) || !probe)
        return vr::VRCompositorError_InvalidTexture;
    probe->Release();
    tex->GetDesc(desc);
    if (!format_ok(desc->Format) || desc->SampleDesc.Count != 1) return vr::VRCompositorError_TextureUsesUnsupportedFormat;
    if (!(desc->BindFlags & D3D11_BIND_SHADER_RESOURCE)) return vr::VRCompositorError_TextureUsesUnsupportedFormat;
    return 0;
}

static void set_ref(ID3D11Texture2D*& slot, ID3D11Texture2D* t) {
    if (t) t->AddRef();
    if (slot) slot->Release();
    slot = t;
}

// Waits until `target` (now_ms clock): a high-resolution waitable timer for all but the last
// half millisecond (Sleep(1) can take 15.6 ms at the default timer resolution), then a spin.
static void sleep_until(double target) {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const double left = target - now_ms();
    if (timer && left > 0.6) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((left - 0.5) * 10000);  // 100 ns units, relative
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
    }
    while (now_ms() < target) YieldProcessor();
}

// --- IVRCompositor ---

class Compositor : public StubIVRCompositor {
public:
    void SetTrackingSpace(vr::ETrackingUniverseOrigin o) override {
        std::lock_guard lock(st().mx);
        logf("SetTrackingSpace(%d)", int(o));
        st().universe = o;
    }
    vr::ETrackingUniverseOrigin GetTrackingSpace() override { return st().universe; }

    vr::EVRCompositorError WaitGetPoses(vr::TrackedDevicePose_t* render, uint32_t n, vr::TrackedDevicePose_t* game, uint32_t gn) override {
        State& s = st();
        const double t_in = now_ms();
        {
            std::lock_guard lock(s.mx);
            if (!s.capture_name.empty()) capture_now();
        }
        poll_command_file();
        tick_script(t_in);
        // Pace: return at the next vsync of the headset's clock.
        const double period = 1000.0 / s.hz;
        if (!s.vsync0) s.vsync0 = t_in;
        const long long k = (long long)std::floor((t_in - s.vsync0) / period) + 1;
        const double target = s.vsync0 + double(k) * period;
        if (s.last_vsync_k >= 0 && k > s.last_vsync_k + 1) s.stat_missed += int(k - s.last_vsync_k - 1);
        s.last_vsync_k = k;
        sleep_until(target);
        const double t_out = now_ms();
        std::lock_guard lock(s.mx);
        advance_poses(t_out);
        ++s.frame;
        s.waited = true;
        s.submitted_this_frame[0] = s.submitted_this_frame[1] = false;
        s.frame_head = s.head;
        for (uint32_t d = 0; d < kDevices; ++d) {
            s.frame_ctl_ok[d] = d != kHmd && s.ctl[d].connected;
            if (s.frame_ctl_ok[d]) s.frame_ctl[d] = controller_aim(d);
        }
        for (uint32_t i = 0; i < n; ++i) {
            if (i < kDevices) fill_pose(i, &render[i]);
            else render[i] = vr::TrackedDevicePose_t{};
        }
        for (uint32_t i = 0; game && i < gn; ++i) {
            if (i < kDevices) fill_pose(i, &game[i]);
            else game[i] = vr::TrackedDevicePose_t{};
        }
        // Stats.
        if (s.last_wait_ms > 0) {
            const double ft = t_out - s.last_wait_ms;
            s.frame_ms_sum += ft;
            s.frame_ms_max = std::max(s.frame_ms_max, ft);
        }
        s.wait_ms_sum += t_out - t_in;
        s.last_wait_ms = t_out;
        ++s.stat_waits;
        stats_tick(t_out);
        return vr::VRCompositorError_None;
    }

    vr::EVRCompositorError GetLastPoses(vr::TrackedDevicePose_t* render, uint32_t n, vr::TrackedDevicePose_t* game, uint32_t gn) override {
        std::lock_guard lock(st().mx);
        for (uint32_t i = 0; i < n; ++i) {
            if (i < kDevices) fill_pose(i, &render[i]);
            else render[i] = vr::TrackedDevicePose_t{};
        }
        for (uint32_t i = 0; game && i < gn; ++i) {
            if (i < kDevices) fill_pose(i, &game[i]);
            else game[i] = vr::TrackedDevicePose_t{};
        }
        return vr::VRCompositorError_None;
    }

    vr::EVRCompositorError GetLastPoseForTrackedDeviceIndex(vr::TrackedDeviceIndex_t d, vr::TrackedDevicePose_t* p, vr::TrackedDevicePose_t* g) override {
        if (d >= vr::k_unMaxTrackedDeviceCount) return vr::VRCompositorError_IndexOutOfRange;
        std::lock_guard lock(st().mx);
        if (p) d < kDevices ? fill_pose(d, p) : void(*p = vr::TrackedDevicePose_t{});
        if (g) d < kDevices ? fill_pose(d, g) : void(*g = vr::TrackedDevicePose_t{});
        return vr::VRCompositorError_None;
    }

    vr::EVRCompositorError Submit(vr::EVREye eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags) override {
        State& s = st();
        std::lock_guard lock(s.mx);
        const int e = submit(eye, tex, bounds, flags);
        if (eye == 0 || eye == 1) {
            s.submit[eye].error = e;
            ++s.stat_submits[eye];
        }
        if (e) note_error(eye == 0 ? "Submit(left)" : "Submit(right)", e, compositor_error_name(e));
        return vr::EVRCompositorError(e);
    }

    void ClearLastSubmittedFrame() override { logf("ClearLastSubmittedFrame"); }
    void PostPresentHandoff() override {
        static int n;
        if (n++ < 3) logf("PostPresentHandoff");
    }
    bool CanRenderScene() override { return true; }
    bool IsFullscreen() override { return true; }
    float GetFrameTimeRemaining() override {
        const State& s = st();
        const double period = 1000.0 / s.hz;
        const double t = now_ms() - s.vsync0;
        return float((period - std::fmod(t, period)) / 1000);
    }

private:
    static int submit(vr::EVREye eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags) {
        State& s = st();
        if (eye != vr::Eye_Left && eye != vr::Eye_Right) return vr::VRCompositorError_IndexOutOfRange;
        D3D11_TEXTURE2D_DESC d{};
        const int e = check_texture(tex, &d);
        if (e) return e;
        if (bounds && (bounds->uMin < 0 || bounds->uMax > 1 || bounds->vMin < 0 || bounds->vMax > 1 || bounds->uMin == bounds->uMax ||
                       bounds->vMin == bounds->vMax))
            return vr::VRCompositorError_InvalidBounds;
        if (!s.waited) {
            static int warned;
            if (warned++ < 3) logf("Submit(eye %d) before the first WaitGetPoses (SteamVR would show nothing for it)", int(eye));
        }
        if (s.submitted_this_frame[eye]) return vr::VRCompositorError_AlreadySubmitted;
        s.submitted_this_frame[eye] = true;
        SubmitRec& r = s.submit[eye];
        set_ref(r.tex, static_cast<ID3D11Texture2D*>(tex->handle));
        r.has_bounds = bounds != nullptr;
        r.bounds = bounds ? *bounds : vr::VRTextureBounds_t{0, 0, 1, 1};
        r.frame = s.frame;
        static int logged[2];
        static UINT last_w[2], last_h[2];
        if (logged[eye] < 3 || d.Width != last_w[eye] || d.Height != last_h[eye]) {
            ++logged[eye];
            logf("Submit(eye %d): %ux%u format %d, bounds %s %.3f %.3f %.3f %.3f, colour space %d, flags %x", int(eye), d.Width, d.Height,
                 int(d.Format), bounds ? "" : "(none)", r.bounds.uMin, r.bounds.vMin, r.bounds.uMax, r.bounds.vMax, int(tex->eColorSpace),
                 unsigned(flags));
            last_w[eye] = d.Width, last_h[eye] = d.Height;
        }
        return 0;
    }
};

// --- IVROverlay ---

static Overlay* by_handle(vr::VROverlayHandle_t h) {
    State& s = st();
    if (h == vr::k_ulOverlayHandleInvalid || h == 0 || h > s.overlays.size()) return nullptr;
    Overlay& o = s.overlays[size_t(h - 1)];
    return o.destroyed ? nullptr : &o;
}

Overlay* find_overlay(const std::string& key) {
    for (Overlay& o : st().overlays)
        if (!o.destroyed && o.key == key) return &o;
    return nullptr;
}

#define OVERLAY_OR_FAIL(h, what)                                       \
    std::lock_guard lock(st().mx);                                     \
    Overlay* o = by_handle(h);                                         \
    if (!o) {                                                          \
        note_error(what, vr::VROverlayError_InvalidHandle, "InvalidHandle"); \
        return vr::VROverlayError_InvalidHandle;                       \
    }

class OverlayImpl : public StubIVROverlay {
public:
    vr::EVROverlayError FindOverlay(const char* key, vr::VROverlayHandle_t* h) override {
        std::lock_guard lock(st().mx);
        Overlay* o = find_overlay(key ? key : "");
        *h = o ? vr::VROverlayHandle_t(o - st().overlays.data() + 1) : vr::k_ulOverlayHandleInvalid;
        return o ? vr::VROverlayError_None : vr::VROverlayError_UnknownOverlay;
    }
    vr::EVROverlayError CreateOverlay(const char* key, const char* name, vr::VROverlayHandle_t* h) override {
        std::lock_guard lock(st().mx);
        const std::string k = key ? key : "", n = name ? name : "";
        vr::EVROverlayError e = vr::VROverlayError_None;
        if (k.size() >= vr::k_unVROverlayMaxKeyLength) e = vr::VROverlayError_KeyTooLong;
        else if (n.size() >= vr::k_unVROverlayMaxNameLength) e = vr::VROverlayError_NameTooLong;
        else if (find_overlay(k)) e = vr::VROverlayError_KeyInUse;
        if (e) {
            note_error("CreateOverlay", e, overlay_error_name(e));
            if (h) *h = vr::k_ulOverlayHandleInvalid;
            return e;
        }
        Overlay o;
        o.key = k, o.name = n;
        st().overlays.push_back(o);
        *h = st().overlays.size();
        logf("CreateOverlay('%s', '%s') -> handle %llu", k.c_str(), n.c_str(), *h);
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError DestroyOverlay(vr::VROverlayHandle_t h) override {
        OVERLAY_OR_FAIL(h, "DestroyOverlay");
        logf("DestroyOverlay %s", o->key.c_str());
        set_ref(o->tex, nullptr);
        o->destroyed = true;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayFlag(vr::VROverlayHandle_t h, vr::VROverlayFlags f, bool on) override {
        OVERLAY_OR_FAIL(h, "SetOverlayFlag");
        if (on) o->flags |= uint64_t(f);
        else o->flags &= ~uint64_t(f);
        logf("SetOverlayFlag %s flag %d = %d", o->key.c_str(), int(f), int(on));
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayFlag(vr::VROverlayHandle_t h, vr::VROverlayFlags f, bool* on) override {
        OVERLAY_OR_FAIL(h, "GetOverlayFlag");
        *on = (o->flags & uint64_t(f)) != 0;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayAlpha(vr::VROverlayHandle_t h, float a) override {
        OVERLAY_OR_FAIL(h, "SetOverlayAlpha");
        o->alpha = a;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t order) override {
        OVERLAY_OR_FAIL(h, "SetOverlaySortOrder");
        o->sort = order;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayTexelAspect(vr::VROverlayHandle_t h, float a) override {
        OVERLAY_OR_FAIL(h, "SetOverlayTexelAspect");
        o->texel_aspect = a;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayWidthInMeters(vr::VROverlayHandle_t h, float w) override {
        OVERLAY_OR_FAIL(h, "SetOverlayWidthInMeters");
        if (!(w > 0)) {
            note_error("SetOverlayWidthInMeters", vr::VROverlayError_InvalidParameter, "InvalidParameter");
            return vr::VROverlayError_InvalidParameter;
        }
        if (w != o->width) logf("SetOverlayWidthInMeters %s %.3f", o->key.c_str(), w);
        o->width = w;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayWidthInMeters(vr::VROverlayHandle_t h, float* w) override {
        OVERLAY_OR_FAIL(h, "GetOverlayWidthInMeters");
        *w = o->width;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayTextureBounds(vr::VROverlayHandle_t h, const vr::VRTextureBounds_t* b) override {
        OVERLAY_OR_FAIL(h, "SetOverlayTextureBounds");
        if (b) o->bounds = *b;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin origin, const vr::HmdMatrix34_t* m) override {
        OVERLAY_OR_FAIL(h, "SetOverlayTransformAbsolute");
        if (!m) return vr::VROverlayError_InvalidParameter;
        if (origin != st().universe) {
            static int warned;
            if (warned++ < 3)
                logf("SetOverlayTransformAbsolute %s: origin %d differs from the scene's tracking space %d (fakevr treats both as one space)",
                     o->key.c_str(), int(origin), int(st().universe));
        }
        const M4 xf = from34(*m);
        if (o->transform != 1 || std::fabs(xf.m[0][3] - o->xf.m[0][3]) + std::fabs(xf.m[1][3] - o->xf.m[1][3]) + std::fabs(xf.m[2][3] - o->xf.m[2][3]) > 0.01)
            logf("SetOverlayTransformAbsolute %s origin %d: at %.3f %.3f %.3f, facing %.3f %.3f %.3f", o->key.c_str(), int(origin), xf.m[0][3], xf.m[1][3],
                 xf.m[2][3], xf.m[0][2], xf.m[1][2], xf.m[2][2]);
        o->transform = 1;
        o->origin = origin;
        o->xf = xf;
        ++o->transform_sets;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin* origin, vr::HmdMatrix34_t* m) override {
        OVERLAY_OR_FAIL(h, "GetOverlayTransformAbsolute");
        if (origin) *origin = o->origin;
        if (m) *m = to34(o->xf);
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayTransformTrackedDeviceRelative(vr::VROverlayHandle_t h, vr::TrackedDeviceIndex_t d, const vr::HmdMatrix34_t* m) override {
        OVERLAY_OR_FAIL(h, "SetOverlayTransformTrackedDeviceRelative");
        logf("SetOverlayTransformTrackedDeviceRelative %s device %u (recorded; captures draw absolute overlays only)", o->key.c_str(), d);
        o->transform = 2;
        if (m) o->xf = from34(*m);
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError ShowOverlay(vr::VROverlayHandle_t h) override {
        OVERLAY_OR_FAIL(h, "ShowOverlay");
        if (!o->visible) logf("ShowOverlay %s", o->key.c_str());
        o->visible = true;
        ++o->shows;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError HideOverlay(vr::VROverlayHandle_t h) override {
        OVERLAY_OR_FAIL(h, "HideOverlay");
        if (o->visible) logf("HideOverlay %s", o->key.c_str());
        o->visible = false;
        ++o->hides;
        return vr::VROverlayError_None;
    }
    bool IsOverlayVisible(vr::VROverlayHandle_t h) override {
        std::lock_guard lock(st().mx);
        Overlay* o = by_handle(h);
        return o && o->visible;
    }
    vr::EVROverlayError SetOverlayTexture(vr::VROverlayHandle_t h, const vr::Texture_t* t) override {
        OVERLAY_OR_FAIL(h, "SetOverlayTexture");
        D3D11_TEXTURE2D_DESC d{};
        if (check_texture(t, &d)) {
            note_error("SetOverlayTexture", vr::VROverlayError_InvalidTexture, "InvalidTexture");
            return vr::VROverlayError_InvalidTexture;
        }
        auto* tex = static_cast<ID3D11Texture2D*>(t->handle);
        if (tex != o->tex) logf("SetOverlayTexture %s: %ux%u format %d (new texture)", o->key.c_str(), d.Width, d.Height, int(d.Format));
        set_ref(o->tex, tex);
        ++o->texture_sets;
        ++st().stat_overlay_tex;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError ClearOverlayTexture(vr::VROverlayHandle_t h) override {
        OVERLAY_OR_FAIL(h, "ClearOverlayTexture");
        set_ref(o->tex, nullptr);
        return vr::VROverlayError_None;
    }
    const char* GetOverlayErrorNameFromEnum(vr::EVROverlayError e) override { return overlay_error_name(e); }
};

static Compositor g_compositor;
static OverlayImpl g_overlay;
vr::IVRCompositor* compositor() { return &g_compositor; }
vr::IVROverlay* overlay() { return &g_overlay; }

// --- stats ---

void stats_tick(double now) {
    State& s = st();
    if (!s.stat_t0) s.stat_t0 = now;
    const double dt = now - s.stat_t0;
    if (dt < 1000) return;
    std::string ov;
    for (const Overlay& o : s.overlays)
        if (!o.destroyed) ov += " " + o.key + (o.visible ? "(shown)" : "(hidden)");
    logf("frames: %.1f s: %d waits (%.1f/s), frame %.2f ms avg %.2f max, wait %.2f ms avg, %d vsyncs missed; submits L %d R %d; overlay textures %d; "
         "errors %d%s%s; overlays%s; haptics %zu",
         dt / 1000, s.stat_waits, s.stat_waits * 1000 / dt, s.stat_waits > 1 ? s.frame_ms_sum / (s.stat_waits - 1) : 0.0, s.frame_ms_max,
         s.stat_waits ? s.wait_ms_sum / s.stat_waits : 0.0, s.stat_missed, s.stat_submits[0], s.stat_submits[1], s.stat_overlay_tex, s.stat_errors,
         s.stat_errors ? ", last: " : "", s.stat_errors ? s.last_error.c_str() : "", ov.empty() ? " none" : ov.c_str(), s.haptics.size());
    s.stat_t0 = now;
    s.stat_waits = s.stat_missed = s.stat_errors = s.stat_overlay_tex = 0;
    s.stat_submits[0] = s.stat_submits[1] = 0;
    s.frame_ms_sum = s.frame_ms_max = s.wait_ms_sum = 0;
}

// --- captures ---

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // BGRA
    uint8_t* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
    const uint8_t* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

static bool read_texture(ID3D11Texture2D* tex, Image* out) {
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                      d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    if (!rgba && !bgra) {
        logf("capture: texture format %d not read", int(d.Format));
        return false;
    }
    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1, sd.ArraySize = 1, sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging))) {
        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            out->w = int(d.Width), out->h = int(d.Height);
            out->px.resize(size_t(d.Width) * d.Height * 4);
            for (UINT y = 0; y < d.Height; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                uint8_t* o = out->at(0, int(y));
                memcpy(o, row, size_t(d.Width) * 4);
                if (rgba)
                    for (UINT x = 0; x < d.Width; ++x) std::swap(o[x * 4], o[x * 4 + 2]);
                if (d.Format == DXGI_FORMAT_B8G8R8X8_UNORM)
                    for (UINT x = 0; x < d.Width; ++x) o[x * 4 + 3] = 255;
            }
            ctx->Unmap(staging, 0);
            ok = true;
        }
        staging->Release();
    }
    ctx->Release();
    dev->Release();
    return ok;
}

static bool save_png(const std::wstring& path, const Image& im, bool keep_alpha) {
    std::vector<uint8_t> px = im.px;
    if (!keep_alpha)
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    IWICStream* s = nullptr;
    IWICBitmapEncoder* e = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) break;
        if (FAILED(f->CreateStream(&s)) || FAILED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
        if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) || FAILED(e->Initialize(s, WICBitmapEncoderNoCache))) break;
        if (FAILED(e->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)) || FAILED(fr->SetSize(UINT(im.w), UINT(im.h)))) break;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(fr->SetPixelFormat(&fmt))) break;
        if (FAILED(fr->WritePixels(UINT(im.h), UINT(im.w) * 4, UINT(px.size()), px.data())) || FAILED(fr->Commit())) break;
        ok = SUCCEEDED(e->Commit());
    } while (false);
    if (fr) fr->Release();
    if (e) e->Release();
    if (s) s->Release();
    if (f) f->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    if (!ok) logf("capture: could not write %ls", path.c_str());
    return ok;
}

static void sample(const Image& im, double u, double v, double out[4]) {
    // Bilinear, clamped; u, v in pixels of `im` (centres at +0.5).
    const double x = std::clamp(u - 0.5, 0.0, double(im.w - 1)), y = std::clamp(v - 0.5, 0.0, double(im.h - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, im.w - 1), y1 = std::min(y0 + 1, im.h - 1);
    const double fx = x - x0, fy = y - y0;
    for (int c = 0; c < 4; ++c) {
        const double a = im.at(x0, y0)[c] * (1 - fx) + im.at(x1, y0)[c] * fx;
        const double b = im.at(x0, y1)[c] * (1 - fx) + im.at(x1, y1)[c] * fx;
        out[c] = (a * (1 - fy) + b * fy) / 255.0;
    }
}

struct EyeCam {
    M4 pose, inv;  // eye -> tracking, tracking -> eye
    double l, r, b, t;
    int w, h;
    // Tracking-space point -> pixel; false behind the eye.
    bool project(const double p[3], double* px, double* py) const {
        double e[3];
        apply_pt(inv, p, e);
        if (e[2] > -1e-3) return false;
        const double tx = e[0] / -e[2], ty = e[1] / -e[2];
        *px = (tx - l) / (r - l) * w;
        *py = (t - ty) / (t - b) * h;
        return true;
    }
};

static void plot(Image& im, int x, int y, const uint8_t c[3], int rad = 1) {
    for (int dy = -rad; dy <= rad; ++dy)
        for (int dx = -rad; dx <= rad; ++dx) {
            const int X = x + dx, Y = y + dy;
            if (X < 0 || Y < 0 || X >= im.w || Y >= im.h) continue;
            uint8_t* p = im.at(X, Y);
            p[0] = c[2], p[1] = c[1], p[2] = c[0], p[3] = 255;
        }
}

// A 3D segment drawn as many projected points (perspective-correct, clipped at the eye).
static void line3(Image& im, const EyeCam& cam, const double a[3], const double b[3], const uint8_t c[3], int steps = 600, int rad = 1, int dash = 0) {
    for (int i = 0; i <= steps; ++i) {
        if (dash && (i / dash) % 2) continue;
        const double f = double(i) / steps;
        const double p[3] = {a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f};
        double x, y;
        if (cam.project(p, &x, &y)) plot(im, int(x), int(y), c, rad);
    }
}

static void cross(Image& im, const EyeCam& cam, const double p[3], const uint8_t c[3], int size) {
    double x, y;
    if (!cam.project(p, &x, &y)) return;
    for (int i = -size; i <= size; ++i) plot(im, int(x) + i, int(y), c, 1), plot(im, int(x), int(y) + i, c, 1);
}

static void draw_truth(Image& im, const EyeCam& cam, const State& s, const std::vector<const Overlay*>& ovs) {
    static const uint8_t cyan[3] = {0, 255, 255}, magenta[3] = {255, 0, 255}, yellow[3] = {255, 230, 0}, green[3] = {0, 255, 0},
                         white[3] = {255, 255, 255};
    // Horizon: level directions at the eye's height, 200 m out, +-85 deg around the head's heading.
    const double ey = cam.pose.m[1][3];
    const double head_yaw = s.frame_head.yaw * kDeg;
    for (int a = -85; a < 85; ++a) {
        const double y0 = head_yaw + a * kDeg, y1 = head_yaw + (a + 1) * kDeg;
        const double p0[3] = {cam.pose.m[0][3] + 200 * std::sin(y0), ey, cam.pose.m[2][3] - 200 * std::cos(y0)};
        const double p1[3] = {cam.pose.m[0][3] + 200 * std::sin(y1), ey, cam.pose.m[2][3] - 200 * std::cos(y1)};
        line3(im, cam, p0, p1, yellow, 40, 1, (a & 1) ? 0 : 0);
    }
    // Controllers: the pointing ray with distance dots, a cross at the tip.
    for (uint32_t d : {kLeft, kRight}) {
        if (!s.frame_ctl_ok[d]) continue;
        const M4& aim = s.frame_ctl[d];
        const uint8_t* c = d == kRight ? cyan : magenta;
        const double o[3] = {aim.m[0][3], aim.m[1][3], aim.m[2][3]};
        const double f[3] = {-aim.m[0][2], -aim.m[1][2], -aim.m[2][2]};
        const double far_[3] = {o[0] + f[0] * 20, o[1] + f[1] * 20, o[2] + f[2] * 20};
        line3(im, cam, o, far_, c, 4000, 0);
        for (double dist : {0.25, 0.5, 1.0, 2.0, 5.0, 10.0}) {
            const double p[3] = {o[0] + f[0] * dist, o[1] + f[1] * dist, o[2] + f[2] * dist};
            double x, y;
            if (cam.project(p, &x, &y)) plot(im, int(x), int(y), c, 4);
        }
        cross(im, cam, o, white, 12);
    }
    // Overlay outlines.
    for (const Overlay* ov : ovs) {
        D3D11_TEXTURE2D_DESC td{};
        ov->tex->GetDesc(&td);
        const double tw = td.Width * (ov->bounds.uMax - ov->bounds.uMin), th = td.Height * (ov->bounds.vMax - ov->bounds.vMin);
        const double hw = ov->width / 2, hh = ov->width * th / tw / ov->texel_aspect / 2;
        const double corners[5][3] = {{-hw, hh, 0}, {hw, hh, 0}, {hw, -hh, 0}, {-hw, -hh, 0}, {-hw, hh, 0}};
        for (int i = 0; i < 4; ++i) {
            double a[3], b[3];
            apply_pt(ov->xf, corners[i], a);
            apply_pt(ov->xf, corners[i + 1], b);
            line3(im, cam, a, b, green, 400, 0);
        }
    }
}

struct OvImage {
    const Overlay* o;
    Image im;
    M4 inv;
    double hw, hh;  // half extents, metres
};

void capture_now() {
    State& s = st();
    const std::string name = s.capture_name;
    s.capture_name.clear();
    const std::wstring wname(name.begin(), name.end());
    logf("capture %s: frame %llu (head %.3f %.3f %.3f yaw %.2f pitch %.2f roll %.2f)", name.c_str(), s.frame, s.frame_head.x, s.frame_head.y,
         s.frame_head.z, s.frame_head.yaw, s.frame_head.pitch, s.frame_head.roll);
    // Overlays: visible, absolute, with a texture; sorted (SteamVR draws higher sort orders on top).
    std::vector<OvImage> ovs;
    std::vector<const Overlay*> drawn;
    for (const Overlay& o : s.overlays) {
        if (o.destroyed || !o.visible || !o.tex || o.transform != 1) continue;
        OvImage oi{&o};
        if (!read_texture(o.tex, &oi.im)) continue;
        save_png(dir_path((wname + L"_ov_" + std::wstring(o.key.begin(), o.key.end()) + L".png").c_str()), oi.im, true);
        const double tw = oi.im.w * (o.bounds.uMax - o.bounds.uMin), th = oi.im.h * (o.bounds.vMax - o.bounds.vMin);
        oi.hw = o.width / 2, oi.hh = o.width * th / tw / o.texel_aspect / 2;
        oi.inv = rigid_inv(o.xf);
        ovs.push_back(std::move(oi));
    }
    std::stable_sort(ovs.begin(), ovs.end(), [](const OvImage& a, const OvImage& b) { return a.o->sort < b.o->sort; });
    for (const OvImage& o : ovs) drawn.push_back(o.o);

    FILE* txt = _wfopen(dir_path((wname + L".txt").c_str()).c_str(), L"w");
    const M4 head = pose_matrix(s.frame_head);
    if (txt) {
        fprintf(txt, "capture %s, frame %llu, universe %d\n", name.c_str(), s.frame, int(s.universe));
        fprintf(txt, "head %.4f %.4f %.4f yaw %.3f pitch %.3f roll %.3f\n", s.frame_head.x, s.frame_head.y, s.frame_head.z, s.frame_head.yaw,
                s.frame_head.pitch, s.frame_head.roll);
        for (uint32_t d : {kLeft, kRight}) {
            if (!s.frame_ctl_ok[d]) {
                fprintf(txt, "%s controller: off\n", d == kLeft ? "left" : "right");
                continue;
            }
            const PoseP p = pose_params(s.frame_ctl[d]);
            fprintf(txt, "%s controller aim (tip) %.4f %.4f %.4f yaw %.3f pitch %.3f roll %.3f\n", d == kLeft ? "left" : "right", p.x, p.y, p.z,
                    p.yaw, p.pitch, p.roll);
        }
    }
    for (int eye = 0; eye < 2; ++eye) {
        EyeCam cam;
        cam.pose = mul(head, eye_to_head(eye));
        cam.inv = rigid_inv(cam.pose);
        eye_tangents(eye, &cam.l, &cam.r, &cam.b, &cam.t);
        const SubmitRec& sr = s.submit[eye];
        Image src;
        const bool have = sr.tex && read_texture(sr.tex, &src);
        // The eye image: the submitted bounds region.
        Image view;
        if (have) {
            const int x0 = int(std::lround(sr.bounds.uMin * src.w)), x1 = int(std::lround(sr.bounds.uMax * src.w));
            const int y0 = int(std::lround(sr.bounds.vMin * src.h)), y1 = int(std::lround(sr.bounds.vMax * src.h));
            const int rw = std::abs(x1 - x0), rh = std::abs(y1 - y0);
            // A tiny texture (the mod's 16x16 black for flat frames): show it at the recommended size.
            const bool tiny = rw < 64 || rh < 64;
            view.w = tiny ? int(s.rec_w) : rw, view.h = tiny ? int(s.rec_h) : rh;
            view.px.assign(size_t(view.w) * view.h * 4, 0);
            Image region;
            region.w = rw, region.h = rh;
            region.px.resize(size_t(rw) * rh * 4);
            for (int y = 0; y < rh; ++y)
                for (int x = 0; x < rw; ++x) {
                    const int sx = x1 >= x0 ? x0 + x : x0 - 1 - x, sy = y1 >= y0 ? y0 + y : y0 - 1 - y;
                    memcpy(region.at(x, y), src.at(sx, sy), 4);
                }
            save_png(dir_path((wname + L"_submit" + std::to_wstring(eye) + L".png").c_str()), region, false);
            for (int y = 0; y < view.h; ++y)
                for (int x = 0; x < view.w; ++x) {
                    double c[4];
                    sample(region, (x + 0.5) * rw / view.w, (y + 0.5) * rh / view.h, c);
                    uint8_t* p = view.at(x, y);
                    for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(c[k] * 255));
                    p[3] = 255;
                }
        } else {
            view.w = int(s.rec_w), view.h = int(s.rec_h);
            view.px.assign(size_t(view.w) * view.h * 4, 0);
            for (size_t i = 3; i < view.px.size(); i += 4) view.px[i] = 255;
        }
        if (s.capture_scale > 0 && s.capture_scale < 1) {
            Image shrunk;
            shrunk.w = std::max(16, int(view.w * s.capture_scale)), shrunk.h = std::max(16, int(view.h * s.capture_scale));
            shrunk.px.resize(size_t(shrunk.w) * shrunk.h * 4);
            for (int y = 0; y < shrunk.h; ++y)
                for (int x = 0; x < shrunk.w; ++x) {
                    double c[4];
                    sample(view, (x + 0.5) * view.w / shrunk.w, (y + 0.5) * view.h / shrunk.h, c);
                    for (int k = 0; k < 4; ++k) shrunk.at(x, y)[k] = uint8_t(std::lround(c[k] * 255));
                }
            view = std::move(shrunk);
        }
        cam.w = view.w, cam.h = view.h;
        // Overlays, per pixel: the eye ray against each overlay's plane.
        int ov_pixels = 0;
        for (const OvImage& oi : ovs) {
            double eo[3] = {0, 0, 0}, origin[3];
            apply_pt(cam.pose, eo, origin);
            double lo[3];
            apply_pt(oi.inv, origin, lo);
            const bool premul = (oi.o->flags & vr::VROverlayFlags_IsPremultiplied) != 0;
            const bool opaque = (oi.o->flags & vr::VROverlayFlags_IgnoreTextureAlpha) != 0;
            const double u0 = oi.o->bounds.uMin * oi.im.w, u1 = oi.o->bounds.uMax * oi.im.w;
            const double v0 = oi.o->bounds.vMin * oi.im.h, v1 = oi.o->bounds.vMax * oi.im.h;
            const M4 to_local = mul(oi.inv, cam.pose);
            for (int y = 0; y < view.h; ++y) {
                const double ty = cam.t - (y + 0.5) / view.h * (cam.t - cam.b);
                for (int x = 0; x < view.w; ++x) {
                    const double tx = cam.l + (x + 0.5) / view.w * (cam.r - cam.l);
                    const double de[3] = {tx, ty, -1};
                    double dl[3];
                    apply_dir(to_local, de, dl);
                    if (std::fabs(dl[2]) < 1e-9) continue;
                    const double k = -lo[2] / dl[2];
                    if (k <= 0) continue;
                    const double px = lo[0] + dl[0] * k, py = lo[1] + dl[1] * k;
                    if (px < -oi.hw || px > oi.hw || py < -oi.hh || py > oi.hh) continue;
                    double c[4];
                    sample(oi.im, u0 + (px / (2 * oi.hw) + 0.5) * (u1 - u0), v0 + (0.5 - py / (2 * oi.hh)) * (v1 - v0), c);
                    const double a = (opaque ? 1.0 : c[3]) * oi.o->alpha;
                    uint8_t* p = view.at(x, y);
                    for (int ch = 0; ch < 3; ++ch) {
                        const double src_c = opaque ? c[ch] * oi.o->alpha : premul ? c[ch] * oi.o->alpha : c[ch] * a;
                        const double v = src_c + p[ch] / 255.0 * (1 - a);
                        p[ch] = uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255));
                    }
                    ++ov_pixels;
                }
            }
        }
        save_png(dir_path((wname + L"_view" + std::to_wstring(eye) + L".png").c_str()), view, false);
        if (s.capture_truth) {
            Image truth = view;
            draw_truth(truth, cam, s, drawn);
            save_png(dir_path((wname + L"_truth" + std::to_wstring(eye) + L".png").c_str()), truth, false);
        }
        if (txt) {
            fprintf(txt, "eye %d: pose %.4f %.4f %.4f, tangents l %.4f r %.4f b %.4f t %.4f; view %dx%d; submit %s", eye, cam.pose.m[0][3],
                    cam.pose.m[1][3], cam.pose.m[2][3], cam.l, cam.r, cam.b, cam.t, view.w, view.h, have ? "" : "NONE");
            if (have)
                fprintf(txt, "%dx%d bounds %s %.4f %.4f %.4f %.4f (frame %llu, error %d)", src.w, src.h, sr.has_bounds ? "" : "(null)",
                        sr.bounds.uMin, sr.bounds.vMin, sr.bounds.uMax, sr.bounds.vMax, sr.frame, sr.error);
            fprintf(txt, "; %d overlay pixels\n", ov_pixels);
            for (uint32_t d : {kLeft, kRight}) {
                if (!s.frame_ctl_ok[d]) continue;
                const M4& aim = s.frame_ctl[d];
                const double o[3] = {aim.m[0][3], aim.m[1][3], aim.m[2][3]};
                double x, y;
                if (cam.project(o, &x, &y)) fprintf(txt, "  eye %d: %s tip at pixel %.1f %.1f\n", eye, d == kLeft ? "left" : "right", x, y);
                for (double dist : {0.5, 1.0, 2.0, 5.0, 10.0, 50.0}) {
                    const double p[3] = {o[0] - aim.m[0][2] * dist, o[1] - aim.m[1][2] * dist, o[2] - aim.m[2][2] * dist};
                    if (cam.project(p, &x, &y))
                        fprintf(txt, "  eye %d: %s ray at %.1f m -> pixel %.1f %.1f\n", eye, d == kLeft ? "left" : "right", dist, x, y);
                }
            }
        }
    }
    if (txt) {
        for (const Overlay& o : s.overlays) {
            if (o.destroyed) continue;
            D3D11_TEXTURE2D_DESC td{};
            if (o.tex) o.tex->GetDesc(&td);
            fprintf(txt, "overlay %s ('%s'): %s, width %.3f m, texture %ux%u, flags %llx, alpha %.2f, sort %u, transform %d origin %d\n", o.key.c_str(),
                    o.name.c_str(), o.visible ? "visible" : "hidden", o.width, td.Width, td.Height, o.flags, o.alpha, o.sort, o.transform, int(o.origin));
            for (int i = 0; i < 3; ++i) fprintf(txt, "  [%8.4f %8.4f %8.4f %8.4f]\n", o.xf.m[i][0], o.xf.m[i][1], o.xf.m[i][2], o.xf.m[i][3]);
        }
        fprintf(txt, "haptics: %zu pulses in all\n", s.haptics.size());
        for (size_t i = s.haptics.size() > 20 ? s.haptics.size() - 20 : 0; i < s.haptics.size(); ++i)
            fprintf(txt, "  %.3f s device %u axis %u %u us\n", s.haptics[i].t_ms / 1000, s.haptics[i].device, s.haptics[i].axis, s.haptics[i].us);
        fprintf(txt, "errors returned so far: %d (last: %s)\n", s.total_errors, s.last_error.c_str());
        fclose(txt);
    }
    logf("capture %s: written (%zu overlays)", name.c_str(), ovs.size());
    // Done marker for tools\fakevr_shot.ps1.
    FILE* done = _wfopen(dir_path((wname + L".done").c_str()).c_str(), L"w");
    if (done) fclose(done);
}

}  // namespace fv
