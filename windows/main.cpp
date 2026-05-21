#ifdef _WIN32
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

#include <windows.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <endpointvolume.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <shellapi.h>

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <cstdint>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

// Dear ImGui
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <d3d11.h>

#define SAFE_RELEASE(punk) \
    if ((punk) != NULL)    \
    {                      \
        (punk)->Release(); \
        (punk) = NULL;     \
    }

// ============================================================
// HELPERS
// ============================================================

std::string WStrToStr(const std::wstring &wstr)
{
    if (wstr.empty())
        return std::string();

    int size_needed = WideCharToMultiByte(
        CP_UTF8,
        0,
        &wstr[0],
        (int)wstr.size(),
        NULL,
        0,
        NULL,
        NULL);

    std::string strTo(size_needed, 0);

    WideCharToMultiByte(
        CP_UTF8,
        0,
        &wstr[0],
        (int)wstr.size(),
        &strTo[0],
        size_needed,
        NULL,
        NULL);

    return strTo;
}

void MuteDevice(IMMDevice *pDevice)
{
    IAudioEndpointVolume *pVolume = nullptr;

    HRESULT hr = pDevice->Activate(
        __uuidof(IAudioEndpointVolume),
        CLSCTX_ALL,
        nullptr,
        (void **)&pVolume);

    if (SUCCEEDED(hr))
    {
        pVolume->SetMute(TRUE, nullptr);
        SAFE_RELEASE(pVolume);
    }
}

void DrawClickableLink(const char *displayText, const char *url)
{
    // Draw text in a hyper-link blue color
    ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "%s", displayText);

    if (ImGui::IsItemHovered())
    {
        // Change the mouse cursor to a pointing hand
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        // Optional: Show the destination URL as a tooltip
        ImGui::SetTooltip("Open in browser:\n%s", url);

        if (ImGui::IsItemClicked())
        {
            ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

// ============================================================
// DATA STRUCTURES
// ============================================================

struct SystemAudioDevice
{
    std::string name;
    IMMDevice *pDevice = nullptr;
    bool selectedAsDestination = false;
};

struct OutputDevice
{
    std::string name;

    IAudioClient *pClient = nullptr;
    IAudioRenderClient *pRender = nullptr;

    std::atomic<bool> enabled{true};

    // Initial volume = 50%
    std::atomic<float> volume{0.5f};

    OutputDevice() = default;

    OutputDevice(
        std::string n,
        IAudioClient *c = nullptr,
        IAudioRenderClient *r = nullptr)
        : name(n),
          pClient(c),
          pRender(r),
          enabled(true),
          volume(0.5f)
    {
    }

    OutputDevice(const OutputDevice &other)
    {
        name = other.name;
        pClient = other.pClient;
        pRender = other.pRender;

        enabled.store(other.enabled.load());
        volume.store(other.volume.load());
    }

    OutputDevice(OutputDevice &&other) noexcept
    {
        name = std::move(other.name);

        pClient = other.pClient;
        pRender = other.pRender;

        enabled.store(other.enabled.load());
        volume.store(other.volume.load());
    }
};

std::atomic<bool> g_IsRunning{false};

// ============================================================
// AUDIO THREAD
// ============================================================

void AudioRoutingThread(
    IAudioClient *pCaptureClient,
    IAudioCaptureClient *pCaptureService,
    std::vector<OutputDevice> &destinations,
    WAVEFORMATEX *pwfx)
{
    pCaptureClient->Start();

    for (auto &dest : destinations)
    {
        if (dest.pClient)
            dest.pClient->Start();
    }

    int channels = pwfx->nChannels;

    while (g_IsRunning.load())
    {

        UINT32 packetSize = 0;

        pCaptureService->GetNextPacketSize(&packetSize);

        if (packetSize != 0)
        {

            BYTE *pData;
            UINT32 framesAvailable;
            DWORD flags;

            pCaptureService->GetBuffer(
                &pData,
                &framesAvailable,
                &flags,
                nullptr,
                nullptr);

            float *pFloatData = reinterpret_cast<float *>(pData);

            int totalSamples = framesAvailable * channels;

            for (auto &dest : destinations)
            {

                if (!dest.pRender)
                    continue;

                BYTE *pRenderData;

                if (SUCCEEDED(dest.pRender->GetBuffer(
                        framesAvailable,
                        &pRenderData)))
                {

                    if (dest.enabled.load())
                    {

                        float currentVolume = dest.volume.load();

                        float *pRenderFloat =
                            reinterpret_cast<float *>(pRenderData);

                        for (int i = 0; i < totalSamples; i++)
                        {
                            pRenderFloat[i] =
                                pFloatData[i] * currentVolume;
                        }
                    }
                    else
                    {
                        memset(
                            pRenderData,
                            0,
                            framesAvailable * pwfx->nBlockAlign);
                    }

                    dest.pRender->ReleaseBuffer(
                        framesAvailable,
                        0);
                }
            }

            pCaptureService->ReleaseBuffer(framesAvailable);
        }
        else
        {
            Sleep(1);
        }
    }

    pCaptureClient->Stop();

    for (auto &dest : destinations)
    {
        if (dest.pClient)
            dest.pClient->Stop();
    }
}

// ============================================================
// DIRECTX / IMGUI
// ============================================================

ID3D11Device *g_pd3dDevice = nullptr;
ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
IDXGISwapChain *g_pSwapChain = nullptr;
ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================
// Assests - Icon as hex array
// Pallete mode = 32 bit RGBA (4bytes/pixel)
// ============================================================

static const uint32_t soundicon_48[] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80808002, 0x60606008, 0x71556312, 0x6a4f6a1d, 0x6e55661e, 0x6d556115, 0x71557109, 0x55555503, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x55555503, 0x70526619, 0x6d55653f, 0x6d526673, 0x6b53679f, 0x6c5467bd, 0x6c5367cf, 0x6b5367df, 0x6c5366e0, 0x6c5367d2, 0x6c5268c0, 0x6c5368a5, 0x6d52677c, 0x6a516748, 0x6b526b1f, 0x80555506, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x60606008, 0x6e526641, 0x6c536799, 0x6b5367d8, 0x6c5367f4, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f7, 0x6b5367df, 0x6c5367a8, 0x6c536853, 0x70506010, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6d496d07, 0x6c536650, 0x6d5367b0, 0x6c5367f1, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x6c5167ff, 0x6c5167ff, 0x6c5267ff, 0x6c5267ff, 0x6c5267ff, 0x6c5367ff, 0x6c5267ff, 0x6c5267ff, 0x6c5167ff, 0x6c5167ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f8, 0x6c5367c1, 0x6c526763, 0x6655660f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6d506623, 0x6d52689b, 0x6c5367f3, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x6c5267ff, 0x6e5b69ff, 0x716f6dff, 0x778d74ff, 0x7ca379ff, 0x7ca57aff, 0x7ca67aff, 0x7ca87aff, 0x7ca67aff, 0x7ca47aff, 0x789275ff, 0x73766fff, 0x6e606aff, 0x6c5367ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f9, 0x6c5467b4, 0x6d556836, 0x80808002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x6c536650, 0x6c5366d6, 0x6c5367fe, 0x6c5367ff, 0x6c5267ff, 0x6c5167ff, 0x6f636bff, 0x789075ff, 0x81bf80ff, 0x85d585ff, 0x87de87ff, 0x87e088ff, 0x87e088ff, 0x87e088ff, 0x87e088ff, 0x87e088ff, 0x87e088ff, 0x87df87ff, 0x86d886ff, 0x83c782ff, 0x7a9d78ff, 0x70696cff, 0x6c5267ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367e6, 0x6d53686c, 0x80555506, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80555506, 0x6b526670, 0x6c5368ea, 0x6c5367ff, 0x6c5367ff, 0x6c5167ff, 0x6e5b69ff, 0x789175ff, 0x82c681ff, 0x87de87ff, 0x87e087ff, 0x87df87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87df87ff, 0x87e087ff, 0x84cc83ff, 0x7a9c78ff, 0x6f646bff, 0x6c5167ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f5, 0x6c53668e, 0x6655660f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x66666605, 0x6c536871, 0x6c5367f4, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x716c6dff, 0x7fb47dff, 0x87dd87ff, 0x87e087ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87df87ff, 0x87df87ff, 0x87df87ff, 0x87df87ff, 0x87df87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87df87ff, 0x87df87ff, 0x82c381ff, 0x747c71ff, 0x6c5567ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fc, 0x6c536693, 0x6655660f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6c52676d, 0x6c5367f4, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x747a70ff, 0x83c882ff, 0x87e087ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87e087ff, 0x87df87ff, 0x86d886ff, 0x84cf84ff, 0x84cc83ff, 0x83ca82ff, 0x84cd83ff, 0x85d384ff, 0x87dd87ff, 0x87e087ff, 0x87df87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x88e188ff, 0x82c381ff, 0x6f616aff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fc, 0x6c53668e, 0x6d496d07, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6d53674d, 0x6c5467e8, 0x6c5367ff, 0x6c5367ff, 0x6c5167ff, 0x73756fff, 0x84ce83ff, 0x87e087ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87e087ff, 0x86d986ff, 0x7fb57eff, 0x778e75ff, 0x73766fff, 0x70666bff, 0x6f636bff, 0x6f616aff, 0x6f636bff, 0x716c6dff, 0x768773ff, 0x7eb07cff, 0x85d685ff, 0x87e087ff, 0x87de87ff, 0x87e087ff, 0x84cf84ff, 0x747d71ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f6, 0x6c536871, 0x80808002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6c556421, 0x6d5366d1, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x70676cff, 0x82c381ff, 0x87e087ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x81c080ff, 0x747e71ff, 0x6d5868ff, 0x6c5166ff, 0x6c5167ff, 0x6c5267ff, 0x6c5267ff, 0x6c5267ff, 0x6c5267ff, 0x6c5167ff, 0x6c5166ff, 0x6d5668ff, 0x73766fff, 0x7fb57eff, 0x86db86ff, 0x84cc83ff, 0x747b70ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5467e8, 0x6c52683b, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80408004, 0x6d536794, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5568ff, 0x7da97bff, 0x87e087ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87dd87ff, 0x7ca57aff, 0x6f606aff, 0x6c5167ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5167ff, 0x6d5a69ff, 0x758172ff, 0x716f6dff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5467ba, 0x71556312, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6a516748, 0x6c5367ef, 0x6c5367ff, 0x6c5367ff, 0x6c5167ff, 0x747b70ff, 0x86d785ff, 0x87df87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x7ca47aff, 0x6d5a69ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5167ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fb, 0x6b546664, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x66666605, 0x6c5367a6, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6d5768ff, 0x7fb37dff, 0x87e088ff, 0x87de87ff, 0x87de87ff, 0x87e087ff, 0x81be80ff, 0x6e5f6aff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x755d70ff, 0x9d8b99ff, 0x72596dff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5267c6, 0x66596614, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x6d526938, 0x6c5367ec, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x716a6cff, 0x85d284ff, 0x87df87ff, 0x87de87ff, 0x87de87ff, 0x86d886ff, 0x747c70ff, 0x6c5167ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x7b6477ff, 0xc9bdc7ff, 0x7d6779ff, 0x6b5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fb, 0x6d54675e, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x6b53678a, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5167ff, 0x778a74ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87e087ff, 0x7fb37dff, 0x6d5768ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x7a6376ff, 0xd4c9d2ff, 0x9b8998ff, 0x6a5065ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5467b4, 0x71557109, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x6b516b13, 0x6c5367ce, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x7da97bff, 0x87e088ff, 0x87de87ff, 0x87de87ff, 0x87df87ff, 0x778e75ff, 0x6c5166ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x7a6376ff, 0xd6cdd5ff, 0xd1c6cfff, 0x836d7fff, 0x6b5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367e4, 0x6b516526, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x6b526632, 0x6c5367ed, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x6d5968ff, 0x81be80ff, 0x87e087ff, 0x87de87ff, 0x87de87ff, 0x86da86ff, 0x73786fff, 0x6c5166ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x7b6476ff, 0xccc1caff, 0xd7cdd6ff, 0xcec3ccff, 0x7f697bff, 0x6b5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fc, 0x6c526757, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x6d54675e, 0x6c5367fe, 0x6c5467ff, 0x6c5568ff, 0x6d5668ff, 0x716e6dff, 0x84d084ff, 0x87df87ff, 0x87de87ff, 0x87df87ff, 0x85d485ff, 0x72716eff, 0x6d5668ff, 0x6d5668ff, 0x6c5567ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x7c6577ff, 0xbbaeb9ff, 0x8b7787ff, 0xcbbfc9ff, 0xbeb1bcff, 0x725a6dff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c53678b, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x6b53678a, 0x6c5267ff, 0x73776fff, 0x7eb07cff, 0x80ba7fff, 0x82c782ff, 0x87dd87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87dc87ff, 0x82c381ff, 0x80bb7fff, 0x7fb67eff, 0x758272ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x7c6577ff, 0xbcaeb9ff, 0x725a6dff, 0x867081ff, 0xd4cad2ff, 0x8f7b8bff, 0x6a5165ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5368b6, 0x80555506, 0x00000000,
    0x00000000, 0x55555503, 0x6b5467ab, 0x6c5367ff, 0x6e5c69ff, 0x7ba079ff, 0x87de87ff, 0x87e087ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87e087ff, 0x87e087ff, 0x7dac7cff, 0x6f626aff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x7c6577ff, 0xbcaeb9ff, 0x745c6fff, 0x6b5266ff, 0xae9fabff, 0xa897a5ff, 0x6a5165ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x6f6372ff, 0x6d5669ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367d2, 0x66596614, 0x00000000,
    0x00000000, 0x71557109, 0x6c5268c0, 0x6c5367ff, 0x6c5267ff, 0x6d5868ff, 0x799677ff, 0x86d886ff, 0x87df87ff, 0x87de87ff, 0x87de87ff, 0x87de87ff, 0x87df87ff, 0x87dd87ff, 0x7ca57aff, 0x6e5c69ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x7c6577ff, 0xbcaeb9ff, 0x745c6fff, 0x6a5165ff, 0x887384ff, 0xab9ba8ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x758088ff, 0x82c5bbff, 0x7a9c9dff, 0x6d596bff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367e1, 0x6e55661e, 0x00000000,
    0x00000000, 0x71557109, 0x6c5268c0, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x6c5467ff, 0x789175ff, 0x86d886ff, 0x87df87ff, 0x87de87ff, 0x87df87ff, 0x86db86ff, 0x7ba079ff, 0x6d5968ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x7c6577ff, 0xbcaeb9ff, 0x745c6fff, 0x6b5266ff, 0x796274ff, 0x9e8c9bff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x6c5568ff, 0x76878dff, 0x85d3c5ff, 0x87e0cfff, 0x87ddccff, 0x7ca4a2ff, 0x6e5d6eff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367e1, 0x6b526b1f, 0x00000000,
    0x00000000, 0x71557109, 0x6c5268c0, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5467ff, 0x768973ff, 0x85d685ff, 0x87e088ff, 0x86d986ff, 0x799376ff, 0x6d5768ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5166ff, 0x6a5065ff, 0x7b6577ff, 0xbcaeb9ff, 0x745c6fff, 0x6b5266ff, 0x745c70ff, 0x8a7586ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5266ff, 0x6d566aff, 0x799799ff, 0x86d8c9ff, 0x87dfceff, 0x87decdff, 0x87decdff, 0x87decdff, 0x7eb2adff, 0x6f6272ff, 0x6c5166ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367e1, 0x6b526b1f, 0x00000000,
    0x00000000, 0x71557109, 0x6c5268c0, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5467ff, 0x768573ff, 0x83c782ff, 0x789275ff, 0x6c5568ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x6e5669ff, 0x816c7dff, 0x806a7cff, 0x7e687aff, 0xbbaeb9ff, 0x745c6fff, 0x6b5266ff, 0x735b6eff, 0x796274ff, 0x6b5266ff, 0x6c5367ff, 0x6c5266ff, 0x6e5b6dff, 0x7ba2a1ff, 0x86dacaff, 0x87dfceff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87dfceff, 0x80bbb3ff, 0x716b79ff, 0x6c5166ff, 0x6c5367ff, 0x6c5367e1, 0x6e55661e, 0x00000000,
    0x00000000, 0x55555503, 0x6b5467ab, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6f636bff, 0x6c5567ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x776072ff, 0xb2a3b0ff, 0xdad0d8ff, 0xd9cfd7ff, 0xc1b4bfff, 0xc1b5bfff, 0x735b6fff, 0x6c5267ff, 0x6d5468ff, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x6f6473ff, 0x7eada9ff, 0x87e0ceff, 0x88e1cfff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87e0ceff, 0x88e2d0ff, 0x82c5bbff, 0x716f7cff, 0x6c5266ff, 0x6d5366d1, 0x66596614, 0x00000000,
    0x00000000, 0x00000000, 0x6b53678a, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0xab9ba8ff, 0xe5dde4ff, 0xe4dce3ff, 0xe4dce3ff, 0xe6dee5ff, 0xcabec8ff, 0x735b6eff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5468ff, 0x758088ff, 0x7ca6a4ff, 0x7ca7a5ff, 0x7fb4aeff, 0x86d9c9ff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x81bfb7ff, 0x7ca6a4ff, 0x7ca5a3ff, 0x747e86ff, 0x6c5367ff, 0x6c5368b6, 0x80555506, 0x00000000,
    0x00000000, 0x00000000, 0x6b52685d, 0x6c5367fd, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x70576bff, 0xc5b8c2ff, 0xe5dde4ff, 0xe3dbe2ff, 0xe3dbe2ff, 0xe5dde4ff, 0xcabec8ff, 0x725a6eff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5266ff, 0x6c5166ff, 0x706675ff, 0x84cfc2ff, 0x87dfceff, 0x87decdff, 0x87decdff, 0x86dacaff, 0x747b84ff, 0x6b5065ff, 0x6c5266ff, 0x6c5266ff, 0x6c5367ff, 0x6c53678b, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x6d536831, 0x6c5367ed, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6e5569ff, 0xbbaeb9ff, 0xe6dee5ff, 0xe3dbe2ff, 0xe3dbe2ff, 0xe5dee4ff, 0xbcafbaff, 0x6e5669ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5166ff, 0x72727eff, 0x86d6c7ff, 0x87decdff, 0x87decdff, 0x87dfceff, 0x85d1c4ff, 0x706977ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fb, 0x6c526757, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x6b516b13, 0x6c5367ce, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x8c7788ff, 0xd6ccd4ff, 0xe5dde4ff, 0xe5dde4ff, 0xd6ccd4ff, 0x897485ff, 0x6b5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5165ff, 0x788f93ff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87e0ceff, 0x81c0b7ff, 0x6e5b6dff, 0x6c5267ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367e4, 0x6e536725, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x6d526788, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x856f80ff, 0xa696a3ff, 0xa695a3ff, 0x867082ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5267ff, 0x6d596bff, 0x80b8b1ff, 0x87e0ceff, 0x87decdff, 0x87decdff, 0x87e0ceff, 0x7ba1a1ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367b3, 0x71557109, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x6b536637, 0x6c5367ec, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5266ff, 0x6a5165ff, 0x6a5165ff, 0x6b5166ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5266ff, 0x76888eff, 0x86dacaff, 0x87decdff, 0x87decdff, 0x87decdff, 0x86d8c9ff, 0x737882ff, 0x6c5166ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fb, 0x6d54675e, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x66666605, 0x6c5367a6, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5165ff, 0x72707cff, 0x84cdc0ff, 0x87dfceff, 0x87decdff, 0x87decdff, 0x87e0cfff, 0x80b9b2ff, 0x6d5a6cff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6b5368c5, 0x6b516b13, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6d546646, 0x6c5367ef, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5266ff, 0x6d576aff, 0x6c5467ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5166ff, 0x72737eff, 0x82c4baff, 0x87dfceff, 0x87decdff, 0x87decdff, 0x87dfceff, 0x85d6c7ff, 0x747b84ff, 0x6c5166ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fb, 0x6c526763, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80408004, 0x6c536693, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5266ff, 0x706574ff, 0x7eaea9ff, 0x799497ff, 0x6e5f70ff, 0x6c5166ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5166ff, 0x6d566aff, 0x758289ff, 0x83cbbfff, 0x87e0ceff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87dfceff, 0x7ba09fff, 0x6c5468ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5467ba, 0x695a6911, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6c556421, 0x6d5366d1, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5166ff, 0x716f7cff, 0x81c1b8ff, 0x87e0ceff, 0x87ddccff, 0x82c3b9ff, 0x778d91ff, 0x6f6473ff, 0x6c5468ff, 0x6c5166ff, 0x6c5165ff, 0x6c5165ff, 0x6c5165ff, 0x6c5165ff, 0x6c5266ff, 0x6d596bff, 0x737681ff, 0x7eaeaaff, 0x86d8c9ff, 0x87dfceff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87dfceff, 0x7fb4aeff, 0x6e5f70ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367e7, 0x6e54653a, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6d52664b, 0x6c5367e7, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5266ff, 0x737680ff, 0x83c8bdff, 0x87e0ceff, 0x87decdff, 0x87decdff, 0x87e0ceff, 0x87ddccff, 0x83cabeff, 0x7dada9ff, 0x79999aff, 0x76898eff, 0x758088ff, 0x76858cff, 0x788f93ff, 0x7ba2a1ff, 0x80bab2ff, 0x85d6c7ff, 0x87e0ceff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87dfceff, 0x7fb5afff, 0x6f6272ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f6, 0x6d53666e, 0x80808002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6d53686c, 0x6c5367f4, 0x6c5367ff, 0x6c5367ff, 0x6c5266ff, 0x75848bff, 0x85d3c5ff, 0x87e0ceff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87dfceff, 0x87e0cfff, 0x87e0ceff, 0x87decdff, 0x87dccbff, 0x87ddccff, 0x87dfceff, 0x87e0cfff, 0x87e0ceff, 0x87dfcdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87dfceff, 0x86dacaff, 0x7da8a5ff, 0x6f6071ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fc, 0x6d53678d, 0x6d496d07, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x66666605, 0x6b526670, 0x6c5367f4, 0x6c5367ff, 0x6c5367ff, 0x6c5468ff, 0x72737eff, 0x7fb5afff, 0x86dbcbff, 0x87e0cfff, 0x87dfcdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87e0ceff, 0x87dfceff, 0x83c9beff, 0x76878dff, 0x6d586aff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367fc, 0x6c546792, 0x6d5b6d0e, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80555506, 0x6b53666b, 0x6c5367e9, 0x6c5367ff, 0x6c5367ff, 0x6c5166ff, 0x6d5a6cff, 0x75828aff, 0x7fb2adff, 0x85d5c6ff, 0x87dfceff, 0x87e0ceff, 0x87dfceff, 0x87dfcdff, 0x87decdff, 0x87decdff, 0x87decdff, 0x87dfcdff, 0x87dfceff, 0x87e0ceff, 0x87e0cfff, 0x87dccbff, 0x82c4baff, 0x799598ff, 0x706675ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f4, 0x6b53678a, 0x6d5b6d0e, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x6b54674f, 0x6c5367d5, 0x6c5367fe, 0x6c5367ff, 0x6c5267ff, 0x6c5165ff, 0x6d576aff, 0x72717dff, 0x7a989aff, 0x80bcb4ff, 0x84cec1ff, 0x85d6c7ff, 0x86d9caff, 0x86d9c9ff, 0x86d9c9ff, 0x85d4c5ff, 0x84cec1ff, 0x82c5bbff, 0x7da9a6ff, 0x758289ff, 0x6e5f70ff, 0x6c5266ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5466e5, 0x6b53666b, 0x80555506, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6d506623, 0x6c53679a, 0x6b5367f2, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5166ff, 0x6c5266ff, 0x6d5a6cff, 0x6f6473ff, 0x72707cff, 0x737680ff, 0x737580ff, 0x737580ff, 0x716d7aff, 0x6f6473ff, 0x6e5e6fff, 0x6c5468ff, 0x6c5165ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f9, 0x6c5367b3, 0x6a526535, 0x80808002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x6d496d07, 0x6b53674a, 0x6c5367aa, 0x6c5367f1, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5266ff, 0x6c5166ff, 0x6c5165ff, 0x6c5165ff, 0x6c5165ff, 0x6c5166ff, 0x6c5266ff, 0x6c5266ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f8, 0x6c5467bd, 0x6d54685b, 0x6d5b6d0e, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x60606008, 0x6c546840, 0x6c536797, 0x6c5367d7, 0x6c5367f3, 0x6c5367fe, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367ff, 0x6c5367f7, 0x6c5367de, 0x6c5266a7, 0x6d546752, 0x6655660f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x55555503, 0x70526619, 0x6d54693d, 0x6c536871, 0x6b53669d, 0x6d5367bc, 0x6c5367cd, 0x6c5367dd, 0x6b5367df, 0x6c5367d0, 0x6c5367bf, 0x6b5267a4, 0x6d54667a, 0x6c536847, 0x6e55661e, 0x80555506, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x60606008, 0x695a6911, 0x6d52641c, 0x6a4f6a1d, 0x6b516b13, 0x71557109, 0x80808002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000};

HICON CreateIconFromRGBA(const uint32_t* srcPixels, int width, int height) {
    // Use the standard BITMAPINFOHEADER (BI_RGB inherently implies BGRA byte order for 32-bit)
    BITMAPINFOHEADER bmi = { 0 };
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = width;
    bmi.biHeight = -height; // Negative means top-down drawing (prevents it from being upside down)
    bmi.biPlanes = 1;
    bmi.biBitCount = 32;
    bmi.biCompression = BI_RGB; 

    HDC hdc = GetDC(NULL);
    uint8_t* pPixels = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdc, (BITMAPINFO*)&bmi, DIB_RGB_COLORS, (void**)&pPixels, NULL, 0);
    ReleaseDC(NULL, hdc);
    
    if (!hBitmap || !pPixels) return NULL;

    // Mathematically unpack the 0xRRGGBBAA hex values and explicitly write them as B, G, R, A
    for (int i = 0; i < width * height; ++i) {
        uint32_t pixel = srcPixels[i];
        
        uint8_t r = (pixel >> 24) & 0xFF;
        uint8_t g = (pixel >> 16) & 0xFF;
        uint8_t b = (pixel >> 8)  & 0xFF;
        uint8_t a =  pixel        & 0xFF;

        // Windows 32-bit DIB expects BGRA order in memory
        pPixels[i * 4 + 0] = b;
        pPixels[i * 4 + 1] = g;
        pPixels[i * 4 + 2] = r;
        pPixels[i * 4 + 3] = a;
    }

    // Windows requires a mask bitmap for CreateIconIndirect, even if we are using the Alpha channel
    HBITMAP hbmMask = CreateBitmap(width, height, 1, 1, NULL);

    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hBitmap;

    HICON hIcon = CreateIconIndirect(&ii);

    // Clean up GDI objects
    DeleteObject(hBitmap);
    DeleteObject(hbmMask);

    return hIcon;
}

// ============================================================
// MAIN
// ============================================================

int main()
{

    HRESULT hr = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED);

    if (FAILED(hr))
        return -1;

    // ========================================================
    // WINDOW
    // ========================================================

    // Generate the HICON from your byte array
    HICON appIcon = CreateIconFromRGBA(soundicon_48, 48, 48);

    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr),
        appIcon,
        LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr,
        L"EchoLink",
        appIcon 
    };

    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"Echo Link",
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        1100,
        720,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    // ========================================================
    // IMGUI SETUP
    // ========================================================

    IMGUI_CHECKVERSION();

    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    (void)io;

    io.IniFilename = nullptr; // Dont create imgui.ini file

    // FONT
    io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeui.ttf",
        18.0f);

    ImGui::StyleColorsDark();

    // ========================================================
    // MODERN THEME
    // ========================================================

    ImGuiStyle &style = ImGui::GetStyle();

    style.WindowRounding = 14.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding = 10.0f;
    style.TabRounding = 10.0f;

    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(10, 8);
    style.ItemSpacing = ImVec2(12, 12);

    ImVec4 *colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);

    colors[ImGuiCol_Header] = ImVec4(0.18f, 0.55f, 0.85f, 0.80f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.60f, 0.95f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.48f, 0.80f, 1.00f);

    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.55f, 0.85f, 0.85f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.62f, 0.95f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.14f, 0.48f, 0.80f, 1.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);

    colors[ImGuiCol_CheckMark] = ImVec4(0.25f, 0.75f, 1.00f, 1.00f);

    colors[ImGuiCol_SliderGrab] =
        ImVec4(0.25f, 0.75f, 1.00f, 1.00f);

    colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.35f, 0.85f, 1.00f, 1.00f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(
        g_pd3dDevice,
        g_pd3dDeviceContext);

    // ========================================================
    // ENUMERATE AUDIO DEVICES
    // ========================================================

    IMMDeviceEnumerator *pEnumerator = nullptr;
    IMMDeviceCollection *pCollection = nullptr;

    std::vector<SystemAudioDevice> allDevices;

    CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&pEnumerator));

    pEnumerator->EnumAudioEndpoints(
        eRender,
        DEVICE_STATE_ACTIVE,
        &pCollection);

    UINT count;

    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; i++)
    {

        SystemAudioDevice sysDev;

        pCollection->Item(i, &sysDev.pDevice);

        IPropertyStore *pProps = nullptr;

        sysDev.pDevice->OpenPropertyStore(
            STGM_READ,
            &pProps);

        PROPVARIANT varName;

        PropVariantInit(&varName);

        pProps->GetValue(
            PKEY_Device_FriendlyName,
            &varName);

        sysDev.name = WStrToStr(varName.pwszVal);

        allDevices.push_back(sysDev);

        PropVariantClear(&varName);

        SAFE_RELEASE(pProps);
    }

    // ========================================================
    // APP STATE
    // ========================================================

    bool isStreaming = false;

    int selectedSourceIdx = 0;
    bool muteSourceDevice = true;

    // 1: Auto-Detect VB-Cable on Startup
    for (size_t i = 0; i < allDevices.size(); ++i)
    {
        if (allDevices[i].name.find("CABLE Output") != std::string::npos ||
            allDevices[i].name.find("VB-Audio") != std::string::npos)
        {
            selectedSourceIdx = (int)i;
            break;
        }
    }

    std::string initializationError = "";

    std::vector<OutputDevice> activeStreamDevices;

    std::thread audioThread;

    IAudioClient *pCaptureClient = nullptr;
    IAudioCaptureClient *pCaptureService = nullptr;

    WAVEFORMATEX *pwfx = nullptr;

    // ========================================================
    // MAIN LOOP
    // ========================================================

    bool done = false;

    while (!done)
    {

        MSG msg;

        while (PeekMessage(
            &msg,
            nullptr,
            0U,
            0U,
            PM_REMOVE))
        {

            TranslateMessage(&msg);
            DispatchMessage(&msg);

            if (msg.message == WM_QUIT)
                done = true;
        }

        if (done)
            break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        ImGui::NewFrame();

        // ====================================================
        // FULLSCREEN WINDOW
        // ====================================================

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);

        ImGui::Begin(
            "EchoLink",
            nullptr,
            ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoTitleBar);

        // ====================================================
        // HEADER
        // ====================================================

        ImGui::TextColored(
            ImVec4(0.25f, 0.75f, 1.0f, 1.0f),
            "EchoLink");

        ImGui::SameLine();

        ImGui::TextDisabled("Real-Time Audio Router");

        // Task 3: Help Button & Modal
        ImGui::SameLine(ImGui::GetWindowWidth() - 70.0f);
        if (ImGui::Button("Help"))
        {
            ImGui::OpenPopup("HelpMenu");
        }

        ImGui::Separator();

        if (ImGui::BeginPopupModal("HelpMenu", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("To stream silently to your Bluetooth devices, install a Virtual Audio Cable.");
            ImGui::Spacing();

            ImGui::Text("Download VB-Cable:");
            DrawClickableLink("https://vb-audio.com/Cable/", "https://vb-audio.com/Cable/");

            ImGui::Spacing();
            ImGui::Text("Instructions:");
            ImGui::Text("1. Install VB-Cable.\n2. Restart PC.\n3. Open EchoLink and select 'CABLE Output' as source.");
            ImGui::Spacing();

            ImGui::TextDisabled("Made by VarunKumar0x10");
            ImGui::SameLine();
            DrawClickableLink("View GitHub Repository", "https://github.com/VarunKumar0x10/EchoLink");

            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }

        ImGui::Spacing();

        // ====================================================
        // CONFIG UI
        // ====================================================

        if (!isStreaming)
        {

            ImGui::Text("1. Select Source Device");

            ImGui::SetNextItemWidth(500);

            if (ImGui::BeginCombo(
                    "##SourceCombo",
                    allDevices[selectedSourceIdx].name.c_str()))
            {

                for (size_t n = 0; n < allDevices.size(); n++)
                {

                    bool is_selected =
                        (selectedSourceIdx == n);

                    if (ImGui::Selectable(
                            allDevices[n].name.c_str(),
                            is_selected))
                    {
                        selectedSourceIdx = n;
                    }

                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }

                ImGui::EndCombo();
            }

            ImGui::Spacing();

            // Task 2: Dynamic UI Instructions & Conditional Muting
            bool isVirtualCable = (allDevices[selectedSourceIdx].name.find("CABLE") != std::string::npos ||
                                   allDevices[selectedSourceIdx].name.find("VB-Audio") != std::string::npos);

            if (isVirtualCable)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                   "Action Required: Set 'CABLE Input' as your Default Output in Windows Sound Settings.");
            }
            else
            {
                ImGui::Checkbox("Mute Local Playback", &muteSourceDevice);
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                   "Action Required: Set this device as your Default Output in Windows.");
            }

            ImGui::Spacing();

            ImGui::Separator();

            ImGui::Spacing();

            ImGui::Text("2. Select Destination Devices");

            ImGui::BeginChild(
                "DestList",
                ImVec2(0, 350),
                true);

            for (size_t i = 0; i < allDevices.size(); i++)
            {

                if (i == selectedSourceIdx)
                    continue;

                ImGui::Checkbox(
                    allDevices[i].name.c_str(),
                    &allDevices[i].selectedAsDestination);
            }

            ImGui::EndChild();

            ImGui::Spacing();

            // =================================================
            // START BUTTON & Validation
            // =================================================

            bool hasDestination = false;
            for (size_t i = 0; i < allDevices.size(); i++)
            {
                if (i != (size_t)selectedSourceIdx && allDevices[i].selectedAsDestination)
                {
                    hasDestination = true;
                    break;
                }
            }

            // 2. Disable the button if no destination is selected
            if (!hasDestination)
            {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button(
                    "Start Audio Routing",
                    ImVec2(-1, 50)))
            {

                initializationError = "";

                bool isVirtualCable = (allDevices[selectedSourceIdx].name.find("CABLE") != std::string::npos ||
                                       allDevices[selectedSourceIdx].name.find("VB-Audio") != std::string::npos);

                if (!isVirtualCable && muteSourceDevice)
                {
                    MuteDevice(allDevices[selectedSourceIdx].pDevice);
                }

                // CAPTURE SETUP

                hr = allDevices[selectedSourceIdx]
                         .pDevice
                         ->Activate(
                             __uuidof(IAudioClient),
                             CLSCTX_ALL,
                             nullptr,
                             (void **)&pCaptureClient);

                if (FAILED(hr))
                {
                    initializationError =
                        "Failed to activate source client.";
                    continue;
                }

                pCaptureClient->GetMixFormat(&pwfx);

                hr = pCaptureClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                    10000000,
                    0,
                    pwfx,
                    nullptr);

                if (FAILED(hr))
                {
                    initializationError =
                        "Failed to initialize source client.";
                    continue;
                }

                pCaptureClient->GetService(
                    __uuidof(IAudioCaptureClient),
                    (void **)&pCaptureService);

                // DESTINATIONS

                for (size_t i = 0; i < allDevices.size(); i++)
                {

                    if (i == selectedSourceIdx)
                        continue;

                    if (!allDevices[i].selectedAsDestination)
                        continue;

                    IAudioClient *pRenderClient = nullptr;
                    IAudioRenderClient *pRenderService = nullptr;

                    allDevices[i].pDevice->Activate(
                        __uuidof(IAudioClient),
                        CLSCTX_ALL,
                        nullptr,
                        (void **)&pRenderClient);

                    hr = pRenderClient->Initialize(
                        AUDCLNT_SHAREMODE_SHARED,
                        0,
                        10000000,
                        0,
                        pwfx,
                        nullptr);

                    if (FAILED(hr))
                    {
                        SAFE_RELEASE(pRenderClient);
                        continue;
                    }

                    pRenderClient->GetService(
                        __uuidof(IAudioRenderClient),
                        (void **)&pRenderService);

                    activeStreamDevices.push_back(
                        OutputDevice(
                            allDevices[i].name,
                            pRenderClient,
                            pRenderService));
                }

                // THREAD

                g_IsRunning.store(true);

                audioThread = std::thread(
                    AudioRoutingThread,
                    pCaptureClient,
                    pCaptureService,
                    std::ref(activeStreamDevices),
                    pwfx);

                isStreaming = true;
            }

            if (!hasDestination)
            {
                ImGui::EndDisabled();

                // 3. Show a helpful hint when the button is disabled
                ImGui::Spacing();
                ImGui::TextColored(
                    ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                    "(!) Please select at least one destination device to begin.");
            }

            if (!initializationError.empty())
            {

                ImGui::Spacing();

                ImGui::TextColored(
                    ImVec4(1, 0.2f, 0.2f, 1),
                    "Error: %s",
                    initializationError.c_str());
            }
        }

        // ====================================================
        // STREAMING UI
        // ====================================================

        else
        {

            ImGui::TextColored(
                ImVec4(0.2f, 1.0f, 0.3f, 1.0f),
                "LIVE");

            ImGui::SameLine();

            ImGui::Text("Streaming audio from:");

            ImGui::TextWrapped(
                "%s",
                allDevices[selectedSourceIdx].name.c_str());

            ImGui::Separator();

            ImGui::Spacing();
            ImGui::BeginChild(
                "OutputsPanel",
                ImVec2(0, -80),
                false);

            for (size_t i = 0; i < activeStreamDevices.size(); i++)
            {

                ImGui::PushID((int)i);

                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);

                // FIX 2: Increased height from 135 to 160 to accommodate
                // the volume slider + the modern theme's padding.
                ImGui::BeginChild(
                    ("DeviceCard" + std::to_string(i)).c_str(),
                    ImVec2(0, 160),
                    true,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);

                ImGui::TextColored(
                    ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                    "%s",
                    activeStreamDevices[i].name.c_str());

                bool isEnabled =
                    activeStreamDevices[i].enabled.load();

                if (ImGui::Checkbox(
                        "Enabled",
                        &isEnabled))
                {
                    activeStreamDevices[i]
                        .enabled
                        .store(isEnabled);
                }

                float volPercent =
                    activeStreamDevices[i]
                        .volume
                        .load() *
                    100.0f;

                ImGui::Text("Volume");

                ImGui::SetNextItemWidth(-1);

                if (ImGui::SliderFloat(
                        "##VolumeSlider",
                        &volPercent,
                        0.0f,
                        100.0f,
                        "%.0f%%"))
                {
                    activeStreamDevices[i]
                        .volume
                        .store(volPercent / 100.0f);
                }

                ImGui::EndChild();

                ImGui::PopStyleVar();

                ImGui::Spacing();

                ImGui::PopID();
            }

            ImGui::EndChild();

            ImGui::Spacing();

            if (ImGui::Button(
                    "Stop Streaming",
                    ImVec2(-1, 50)))
            {

                g_IsRunning.store(false);

                if (audioThread.joinable())
                {
                    audioThread.join();
                }

                SAFE_RELEASE(pCaptureClient);
                SAFE_RELEASE(pCaptureService);

                for (auto &dev : activeStreamDevices)
                {

                    SAFE_RELEASE(dev.pClient);
                    SAFE_RELEASE(dev.pRender);
                }

                activeStreamDevices.clear();

                if (pwfx)
                {
                    CoTaskMemFree(pwfx);
                    pwfx = nullptr;
                }

                isStreaming = false;
            }
        }

        ImGui::End();

        // ====================================================
        // RENDER
        // ====================================================

        ImGui::Render();

        const float clear_color_with_alpha[4] = {
            0.06f,
            0.07f,
            0.09f,
            1.0f};

        g_pd3dDeviceContext->OMSetRenderTargets(
            1,
            &g_mainRenderTargetView,
            nullptr);

        g_pd3dDeviceContext->ClearRenderTargetView(
            g_mainRenderTargetView,
            clear_color_with_alpha);

        ImGui_ImplDX11_RenderDrawData(
            ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    // ========================================================
    // CLEANUP
    // ========================================================

    if (isStreaming)
    {

        g_IsRunning.store(false);

        if (audioThread.joinable())
            audioThread.join();

        SAFE_RELEASE(pCaptureClient);
        SAFE_RELEASE(pCaptureService);

        for (auto &dev : activeStreamDevices)
        {
            SAFE_RELEASE(dev.pClient);
            SAFE_RELEASE(dev.pRender);
        }

        if (pwfx)
            CoTaskMemFree(pwfx);
    }

    for (auto &d : allDevices)
        SAFE_RELEASE(d.pDevice);

    SAFE_RELEASE(pEnumerator);
    SAFE_RELEASE(pCollection);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();

    ImGui::DestroyContext();

    CleanupDeviceD3D();

    DestroyWindow(hwnd);

    UnregisterClassW(
        wc.lpszClassName,
        wc.hInstance);

    if (appIcon)
    {
        DestroyIcon(appIcon);
    }

    CoUninitialize();

    return 0;
}

// ============================================================
// WIN32 / DIRECTX IMPLEMENTATION
// ============================================================

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam);

LRESULT WINAPI WndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{

    if (ImGui_ImplWin32_WndProcHandler(
            hWnd,
            msg,
            wParam,
            lParam))
        return true;

    switch (msg)
    {

    case WM_SIZE:

        if (
            g_pd3dDevice != nullptr &&
            wParam != SIZE_MINIMIZED)
        {

            CleanupRenderTarget();

            g_pSwapChain->ResizeBuffers(
                0,
                (UINT)LOWORD(lParam),
                (UINT)HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN,
                0);

            CreateRenderTarget();
        }

        return 0;

    case WM_SYSCOMMAND:

        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;

        break;

    case WM_DESTROY:

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(
        hWnd,
        msg,
        wParam,
        lParam);
}

bool CreateDeviceD3D(HWND hWnd)
{

    DXGI_SWAP_CHAIN_DESC sd;

    ZeroMemory(&sd, sizeof(sd));

    sd.BufferCount = 2;

    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;

    sd.BufferDesc.Format =
        DXGI_FORMAT_R8G8B8A8_UNORM;

    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;

    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    sd.OutputWindow = hWnd;

    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;

    sd.Windowed = TRUE;

    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;

    D3D_FEATURE_LEVEL featureLevel;

    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext);

    if (res == DXGI_ERROR_UNSUPPORTED)
    {

        res = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            createDeviceFlags,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &g_pSwapChain,
            &g_pd3dDevice,
            &featureLevel,
            &g_pd3dDeviceContext);
    }

    if (res != S_OK)
        return false;

    CreateRenderTarget();

    return true;
}

void CleanupDeviceD3D()
{

    CleanupRenderTarget();

    if (g_pSwapChain)
    {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }

    if (g_pd3dDeviceContext)
    {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }

    if (g_pd3dDevice)
    {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

void CreateRenderTarget()
{

    ID3D11Texture2D *pBackBuffer;

    g_pSwapChain->GetBuffer(
        0,
        IID_PPV_ARGS(&pBackBuffer));

    if (pBackBuffer != nullptr)
    {

        g_pd3dDevice->CreateRenderTargetView(
            pBackBuffer,
            nullptr,
            &g_mainRenderTargetView);

        pBackBuffer->Release();
    }
}

void CleanupRenderTarget()
{

    if (g_mainRenderTargetView)
    {

        g_mainRenderTargetView->Release();

        g_mainRenderTargetView = nullptr;
    }
}