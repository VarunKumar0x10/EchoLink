#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <endpointvolume.h> // Required for muting the device
#include <Functiondiscoverykeys_devpkey.h> // Required for friendly names
#include <iostream>
#include <vector>
#include <string>
#include <thread>

// Helper macros
#define SAFE_RELEASE(punk)  if ((punk) != NULL) { (punk)->Release(); (punk) = NULL; }

void MuteDevice(IMMDevice* pDevice) {
    IAudioEndpointVolume* pVolume = nullptr;
    HRESULT hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&pVolume);
    if (SUCCEEDED(hr)) {
        pVolume->SetMute(TRUE, nullptr);
        std::wcout << L"[System] Source device muted successfully." << std::endl;
        SAFE_RELEASE(pVolume);
    } else {
        std::wcout << L"[Warning] Could not mute source device." << std::endl;
    }
}

int main() {
    HRESULT hr;
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return -1;

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDeviceCollection* pCollection = nullptr;
    std::vector<IMMDevice*> devices; // Store the actual device pointers

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator));
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);

    UINT count;
    pCollection->GetCount(&count);
    std::wcout << L"--- Detected Active Audio Devices ---" << std::endl;

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDevice = nullptr;
        pCollection->Item(i, &pDevice);
        devices.push_back(pDevice); // Keep device for later use

        IPropertyStore* pProps = nullptr;
        pDevice->OpenPropertyStore(STGM_READ, &pProps);
        PROPVARIANT varName;
        PropVariantInit(&varName);
        pProps->GetValue(PKEY_Device_FriendlyName, &varName);
        
        std::wcout << L"[" << i << L"] : " << varName.pwszVal << std::endl;

        PropVariantClear(&varName);
        SAFE_RELEASE(pProps);
    }

    // --- PHASE 1: COLLECT INPUTS ---
    int sourceIndex;
    std::wcout << L"\nEnter the Index for the SOURCE device (to capture from): ";
    std::cin >> sourceIndex;

    if (sourceIndex < 0 || sourceIndex >= count) {
        std::wcout << L"Invalid Source Index. Exiting." << std::endl;
        return -1;
    }

    IMMDevice* pSrcDevice = devices[sourceIndex];
    MuteDevice(pSrcDevice); // MUTE THE SOURCE DEVICE

    int numToStream;
    std::wcout << L"\nHow many destination devices to stream to? ";
    std::cin >> numToStream;

    std::vector<int> destIndices;
    for (int i = 0; i < numToStream; ++i) {
        int idx;
        std::wcout << L"Enter Index for Destination Device #" << (i + 1) << L": ";
        std::cin >> idx;
        if (idx >= 0 && idx < (int)count && idx != sourceIndex) {
            destIndices.push_back(idx);
        } else {
            std::wcout << L"Invalid or duplicate index ignored." << std::endl;
        }
    }

    // --- PHASE 2: INITIALIZE WASAPI PIPELINE ---
    IAudioClient* pCaptureClient = nullptr;
    IAudioCaptureClient* pCaptureService = nullptr;
    WAVEFORMATEX* pwfx = nullptr;

    // 1. Setup Loopback Source
    pSrcDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pCaptureClient);
    pCaptureClient->GetMixFormat(&pwfx); // Crucial: Get system format
    pCaptureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, pwfx, nullptr);
    pCaptureClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureService);

    // 2. Setup All Destinations
    std::vector<IAudioClient*> renderClients;
    std::vector<IAudioRenderClient*> renderServices;

    for (int destIdx : destIndices) {
        IMMDevice* pDestDevice = devices[destIdx];
        IAudioClient* pRenderClient = nullptr;
        IAudioRenderClient* pRenderService = nullptr;

        pDestDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pRenderClient);
        
        // Force the destination to use the SOURCE'S format
        hr = pRenderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pwfx, nullptr);
        if (FAILED(hr)) {
            std::wcout << L"[Error] Device [" << destIdx << L"] does not support the source format. Skipping." << std::endl;
            SAFE_RELEASE(pRenderClient);
            continue;
        }

        pRenderClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderService);
        
        renderClients.push_back(pRenderClient);
        renderServices.push_back(pRenderService);
    }

    // --- PHASE 3: EXECUTE STREAMING ---
    std::wcout << L"\nStarting simultaneous streams... Press Ctrl+C to stop." << std::endl;
    
    pCaptureClient->Start();
    for (auto client : renderClients) {
        client->Start();
    }

    // 1-to-Many Distribution Loop
    while (true) {
        UINT32 packetSize = 0;
        pCaptureService->GetNextPacketSize(&packetSize);
        
        if (packetSize != 0) {
            BYTE* pData;
            UINT32 framesAvailable;
            DWORD flags;
            
            // Get audio from Source
            pCaptureService->GetBuffer(&pData, &framesAvailable, &flags, nullptr, nullptr);
            
            // Distribute to ALL Destinations simultaneously
            for (auto pRenderService : renderServices) {
                BYTE* pRenderData;
                hr = pRenderService->GetBuffer(framesAvailable, &pRenderData);
                if (SUCCEEDED(hr)) {
                    memcpy(pRenderData, pData, framesAvailable * pwfx->nBlockAlign);
                    pRenderService->ReleaseBuffer(framesAvailable, 0);
                }
            }

            // Release Source Buffer
            pCaptureService->ReleaseBuffer(framesAvailable);
        } else {
            Sleep(1); // Rest CPU
        }
    }

    // (Cleanup omitted for brevity, process termination will release resources)
    return 0;
}