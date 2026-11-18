// The fake SteamVR runtime (src/fakevr) through the OpenVR header, as the mod calls it, plus the
// mod's own OpenVR conversions (src/vr/xform, aim) fed with what the fake reports: head turns,
// tilts and leans arrive in the game camera's space with the right signs, the asymmetric eye
// tangents survive the matrix round trip, the controller's tip gives the scripted pointing ray.
// Runs from out\fakevr (next to the fake openvr_api.dll); writes its log/captures to out\fakevr\hovr.
#include <windows.h>
#include <d3d11.h>
#include <openvr.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/vr/aim.h"
#include "../src/vr/xform.h"

using namespace hv::vr;

static int g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                      \
    do {                                                                                           \
        const double va = (a), vb = (b);                                                           \
        if (!(std::fabs(va - vb) <= (tol))) {                                                      \
            std::printf("%s:%d: %s = %.9g, expected %s = %.9g\n", __FILE__, __LINE__, #a, va, #b, vb); \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

extern "C" __declspec(dllimport) void __cdecl fakevr_command(const char* line);

static const double kDeg = 3.14159265358979 / 180;

static Mat4 wait_head(::vr::TrackedDevicePose_t* poses) {
    ::vr::VRCompositor()->WaitGetPoses(poses, ::vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    return from_rows34(poses[::vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m);
}

int main() {
    ::vr::EVRInitError err = ::vr::VRInitError_Unknown;
    ::vr::IVRSystem* sys = ::vr::VR_Init(&err, ::vr::VRApplication_Scene);
    CHECK(sys && err == ::vr::VRInitError_None);
    if (!sys) return 1;
    char runtime[64] = {};
    CHECK(std::string(sys->GetRuntimeVersion()) == "fakevr 1");

    uint32_t w = 0, h = 0;
    sys->GetRecommendedRenderTargetSize(&w, &h);
    CHECK(w == 2064 && h == 2016);

    // Asymmetric tangents through the matrix, the way vrsys.cpp reads them.
    for (int eye = 0; eye < 2; ++eye) {
        const ::vr::HmdMatrix44_t p = sys->GetProjectionMatrix(::vr::EVREye(eye), 0.1f, 100.f);
        float t[4];
        CHECK(tangents_from_openvr(p.m, t));
        const double outer = std::tan(52 * kDeg), inner = std::tan(45 * kDeg);
        CHECK_NEAR(t[0], eye == 0 ? -outer : -inner, 1e-4);
        CHECK_NEAR(t[1], eye == 0 ? inner : outer, 1e-4);
        CHECK_NEAR(t[2], -std::tan(50 * kDeg), 1e-4);  // bottom (down 50)
        CHECK_NEAR(t[3], std::tan(46 * kDeg), 1e-4);   // top (up 46)
        const Mat4 e = from_rows34(sys->GetEyeToHeadTransform(::vr::EVREye(eye)).m);
        CHECK_NEAR(e.m[0][3], eye == 0 ? -0.032 : 0.032, 1e-6);
    }

    ::vr::VRCompositor()->SetTrackingSpace(::vr::TrackingUniverseStanding);
    ::vr::TrackedDevicePose_t poses[::vr::k_unMaxTrackedDeviceCount];

    // Pacing: ~120 Hz.
    fakevr_command("head 0 1.6 0 0 0 0");
    wait_head(poses);
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 24; ++i) wait_head(poses);
    QueryPerformanceCounter(&b);
    const double ms = double(b.QuadPart - a.QuadPart) * 1000 / double(f.QuadPart) / 24;
    CHECK_NEAR(ms, 1000.0 / 120, 1.0);

    // Recentre at yaw 0; then turn, tilt and lean: the mod's head-in-game pose.
    const Mat4 anchor = yaw_only_rh(wait_head(poses));
    fakevr_command("head 0 1.6 0 30 0 0");
    Mat4 g = head_in_game(anchor, wait_head(poses));
    CHECK_NEAR(g.m[0][2], std::sin(30 * kDeg), 1e-4);  // forward (+z, game camera) towards +x: turned right
    fakevr_command("head 0 1.6 0 0 20 0");
    g = head_in_game(anchor, wait_head(poses));
    CHECK_NEAR(g.m[1][2], std::sin(20 * kDeg), 1e-4);  // forward up
    fakevr_command("head 0 1.6 0 0 0 10");
    g = head_in_game(anchor, wait_head(poses));
    CHECK_NEAR(g.m[1][0], -std::sin(10 * kDeg), 1e-4);  // right ear down: the head's +x axis points down
    fakevr_command("head 0.1 1.7 -0.2 0 0 0");
    g = head_in_game(anchor, wait_head(poses));
    CHECK_NEAR(g.m[0][3], 0.1, 1e-5);  // moved right
    CHECK_NEAR(g.m[1][3], 0.1, 1e-5);  // up
    CHECK_NEAR(g.m[2][3], 0.2, 1e-5);  // forward (game +z)
    // A recentre after turning: the anchor follows the yaw only.
    fakevr_command("head 0 1.6 0 40 15 5");
    const Mat4 turned = wait_head(poses);
    const Mat4 a2 = yaw_only_rh(turned);
    g = head_in_game(a2, turned);
    CHECK_NEAR(g.m[0][2], 0, 1e-4);                     // looking straight ahead again
    CHECK_NEAR(g.m[1][2], std::sin(15 * kDeg), 1e-4);   // the pitch stays

    // Controllers: the scripted pointing pose is raw x tip.
    fakevr_command("ctl R 0.2 1.4 -0.3 10 -5 0");
    wait_head(poses);
    const ::vr::TrackedDeviceIndex_t right = sys->GetTrackedDeviceIndexForControllerRole(::vr::TrackedControllerRole_RightHand);
    CHECK(right == 2);
    char model[128] = {};
    sys->GetStringTrackedDeviceProperty(right, ::vr::Prop_RenderModelName_String, model, sizeof model);
    CHECK(std::string(model) == "oculus_quest2_controller_right");
    ::vr::RenderModel_ComponentState_t comp{};
    ::vr::VRControllerState_t cs{};
    ::vr::RenderModel_ControllerMode_State_t mode{};
    CHECK(::vr::VRRenderModels()->GetComponentState(model, ::vr::k_pch_Controller_Component_Tip, &cs, &mode, &comp));
    const Mat4 tip = from_rows34(comp.mTrackingToComponentLocal.m);
    CHECK_NEAR(std::asin(-tip.m[1][2]) / kDeg, -40, 1e-3);  // pointing 40 deg below the raw axis
    const Mat4 aim = mul(from_rows34(poses[right].mDeviceToAbsoluteTracking.m), tip);
    const Ray r = pose_ray_rh(aim);
    CHECK_NEAR(r.o[0], 0.2, 1e-4);
    CHECK_NEAR(r.o[1], 1.4, 1e-4);
    CHECK_NEAR(r.d[0], std::sin(10 * kDeg) * std::cos(5 * kDeg), 1e-4);
    CHECK_NEAR(r.d[1], -std::sin(5 * kDeg), 1e-4);

    // Buttons: a timed trigger press shows in the state, on the button and axis 1.
    fakevr_command("btn R trigger 500");
    CHECK(sys->GetControllerState(right, &cs, sizeof cs));
    CHECK((cs.ulButtonPressed & ::vr::ButtonMaskFromId(::vr::k_EButton_SteamVR_Trigger)) != 0);
    CHECK_NEAR(cs.rAxis[1].x, 1, 1e-6);
    fakevr_command("release R all");
    CHECK(sys->GetControllerState(right, &cs, sizeof cs));
    CHECK(cs.ulButtonPressed == 0);
    // Axes are not described by default (as the Steam Frame's own driver).
    ::vr::ETrackedPropertyError pe;
    sys->GetInt32TrackedDeviceProperty(right, ::vr::Prop_Axis1Type_Int32, &pe);
    CHECK(pe != ::vr::TrackedProp_Success);

    // Submit and overlays with a real D3D11 texture (WARP), and a capture.
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    CHECK(SUCCEEDED(hr));
    if (SUCCEEDED(hr)) {
        std::vector<uint32_t> px(256 * 128, 0xff204080);
        D3D11_TEXTURE2D_DESC d{256, 128, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init{px.data(), 256 * 4, 0};
        ID3D11Texture2D* tex = nullptr;
        dev->CreateTexture2D(&d, &init, &tex);
        wait_head(poses);
        ::vr::Texture_t t{tex, ::vr::TextureType_DirectX, ::vr::ColorSpace_Gamma};
        const ::vr::VRTextureBounds_t left{0, 0, 0.5f, 1}, right_half{0.5f, 0, 1, 1}, bad{0, 0, 1.5f, 1};
        CHECK(::vr::VRCompositor()->Submit(::vr::Eye_Left, &t, &bad) == ::vr::VRCompositorError_InvalidBounds);
        CHECK(::vr::VRCompositor()->Submit(::vr::Eye_Left, &t, &left) == ::vr::VRCompositorError_None);
        CHECK(::vr::VRCompositor()->Submit(::vr::Eye_Left, &t, &left) == ::vr::VRCompositorError_AlreadySubmitted);
        CHECK(::vr::VRCompositor()->Submit(::vr::Eye_Right, &t, &right_half) == ::vr::VRCompositorError_None);
        ::vr::VROverlayHandle_t ov = ::vr::k_ulOverlayHandleInvalid;
        CHECK(::vr::VROverlay()->CreateOverlay("test.panel", "test panel", &ov) == ::vr::VROverlayError_None);
        CHECK(::vr::VROverlay()->CreateOverlay("test.panel", "again", &ov) == ::vr::VROverlayError_KeyInUse);
        CHECK(::vr::VROverlay()->FindOverlay("test.panel", &ov) == ::vr::VROverlayError_None);
        ::vr::VROverlay()->SetOverlayWidthInMeters(ov, 1.0f);
        ::vr::VROverlay()->SetOverlayTexture(ov, &t);
        const Mat4 at = translation(0, 1.6f, -1.5f);
        ::vr::HmdMatrix34_t m;
        to_rows34(at, m.m);
        ::vr::VROverlay()->SetOverlayTransformAbsolute(ov, ::vr::TrackingUniverseStanding, &m);
        ::vr::VROverlay()->ShowOverlay(ov);
        CHECK(::vr::VROverlay()->IsOverlayVisible(ov));
        fakevr_command("capture unit scale 0.25");
        fakevr_command("head 0 1.6 0 0 0 0");
        wait_head(poses);  // the capture happens here
        DWORD attr = GetFileAttributesW(L"hovr\\fakevr\\unit_view0.png");
        CHECK(attr != INVALID_FILE_ATTRIBUTES);
        CHECK(GetFileAttributesW(L"hovr\\fakevr\\unit_truth1.png") != INVALID_FILE_ATTRIBUTES);
        CHECK(GetFileAttributesW(L"hovr\\fakevr\\unit_ov_test.panel.png") != INVALID_FILE_ATTRIBUTES);
        // Haptics are recorded.
        sys->TriggerHapticPulse(right, 0, 2000);
        tex->Release();
        ctx->Release();
        dev->Release();
    }
    ::vr::VR_Shutdown();
    (void)runtime;
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
