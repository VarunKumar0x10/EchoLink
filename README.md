# EchoLink 

EchoLink is a real-time, low-latency audio routing application for Windows. It captures system audio from a source playback device using WASAPI loopback and simultaneously broadcasts it to multiple destination audio endpoints. It features a modern, GPU-accelerated interface powered by Dear ImGui and DirectX 11.

**Developed by:** [VarunKumar0x10](https://github.com/VarunKumar0x10)

##  Features

* **Real-Time Audio Routing:** Stream synchronized audio to multiple output devices simultaneously (e.g., multiple Bluetooth headphones or speakers).
* **Modern UI:** A clean, dark-themed interface built with Dear ImGui, featuring smooth window rounding, custom colors, and an intuitive layout.
* **Smart Source Detection:** Automatically detects Virtual Audio Cables (like VB-Cable) on startup and selects them as the default source.
* **Dynamic Muting:** Intelligently mutes physical source devices to prevent double-audio playback, while providing contextual warnings when configuring virtual cables.
* **Per-Device Control:** Independent volume sliders and enable/disable toggles for every active destination stream.
* **Low Overhead:** Built in C++ utilizing hardware-accelerated DirectX 11 rendering and efficient WASAPI multi-threading.

##  Prerequisites

To build EchoLink from source, you will need the following development tools:

* **OS:** Windows 10 / Windows 11
* **Compiler:** MSVC (Visual Studio 2026 v18 or newer recommended)
* **Build System:** CMake (v3.15+) and NMake
* **Environment:** Developer PowerShell for VS
* **Dependencies:** * Windows SDK (for `dwmapi.lib`, `mmdeviceapi`, `Audioclient`)
* [Dear ImGui](https://github.com/ocornut/imgui) (with Win32 and DX11 backends)


* **Optional (but highly recommended):** [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) for silent loopback streaming.

##  Building the Project

1. Open the **Developer PowerShell for VS**.
2. Clone the repository and navigate to the project directory:
```powershell
git clone https://github.com/Dinoking/EchoLink.git
cd EchoLink

```


3. Generate the build files using CMake:
```powershell
mkdir build
cd build
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release

```


4. Compile the executable (this will also compile the embedded icon resources):
```powershell
cmake --build . --config Release

```


5. The compiled `EchoLink.exe` will be located in your `build` directory.

##  Usage Guide

### Method 1: Silent Streaming (Recommended)

This method allows you to stream audio to multiple Bluetooth devices without audio playing out of your PC's physical speakers.

1. Install [VB-Cable](https://vb-audio.com/Cable/) and restart your computer.
2. In Windows Sound Settings, set **CABLE Input** as your Default Playback Device.
3. Launch `EchoLink.exe`. It will automatically select **CABLE Output** as the source.
4. Check the boxes next to the Bluetooth headphones or speakers you want to stream to.
5. Click **Start Audio Routing**.

### Method 2: Physical Device Loopback

1. Launch `EchoLink.exe`.
2. Select your physical speakers/headphones from the dropdown.
3. Leave **"Mute Local Playback"** checked (unless you want audio to come out of the source device *and* the destination devices).
4. Select your destination devices and click **Start**.
