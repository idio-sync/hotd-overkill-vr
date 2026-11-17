// mute=1 (default): mutes this process's Windows audio sessions, so test runs are silent
// without touching the game's own volume settings. Re-checked for new sessions.
// (From the killer7-vr probe.)
#include "common.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>

namespace hv {

static void mute_sessions() {
    IMMDeviceEnumerator* devices = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager2* manager = nullptr;
    IAudioSessionEnumerator* sessions = nullptr;
    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices)))) break;
        if (FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device))) break;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&manager))))
            break;
        if (FAILED(manager->GetSessionEnumerator(&sessions))) break;
        int count = 0;
        sessions->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* control = nullptr;
            if (FAILED(sessions->GetSession(i, &control))) continue;
            IAudioSessionControl2* control2 = nullptr;
            DWORD pid = 0;
            if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&control2)))) {
                control2->GetProcessId(&pid);
                control2->Release();
            }
            if (pid == GetCurrentProcessId()) {
                ISimpleAudioVolume* volume = nullptr;
                if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&volume)))) {
                    BOOL already = FALSE;
                    volume->GetMute(&already);
                    if (!already) {
                        volume->SetMute(TRUE, nullptr);
                        logf("muted audio session %d", i);
                    }
                    volume->Release();
                }
            }
            control->Release();
        }
    } while (false);
    if (sessions) sessions->Release();
    if (manager) manager->Release();
    if (device) device->Release();
    if (devices) devices->Release();
}

static DWORD WINAPI mute_thread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (int i = 0;; ++i) {
        mute_sessions();
        Sleep(i < 40 ? 250 : 2000);  // quickly at boot, before the logo jingles
    }
}

void start_audio_mute() {
    if (!harness_int(L"mute", 1, 0)) return;  // release: sound on
    CloseHandle(CreateThread(nullptr, 0, mute_thread, nullptr, 0, nullptr));
}

}  // namespace hv
