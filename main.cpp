#ifdef _WIN32
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

#include <windows.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <endpointvolume.h>
#include <Functiondiscoverykeys_devpkey.h>
#include "resource.h"

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#pragma comment(lib, "dwmapi.lib")

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

    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        GetModuleHandle(nullptr),
        LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON1)),
        LoadCursor(nullptr, IDC_ARROW),                                 
        nullptr,
        nullptr,
        L"EchoLink",
        LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON1))  
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
            ImGui::InputText("##vblink", (char*)"https://vb-audio.com/Cable/", 64, ImGuiInputTextFlags_ReadOnly);
            
            ImGui::Spacing();
            ImGui::Text("Instructions:");
            ImGui::Text("1. Install VB-Cable.\n2. Restart PC.\n3. Open EchoLink and select 'CABLE Output' as source.");
            ImGui::Spacing();
            
            ImGui::TextDisabled("Made by Dinoking");
            ImGui::TextDisabled("GitHub: https://github.com/Dinoking/EchoLink");
            
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
                    "(!) Please select at least one destination device to begin."
                );
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