// EchoLink For Linux
// Developed by VarunKumar0x10
// https://github.com/VarunKumar0x10/EchoLink

#include <pulse/simple.h>
#include <pulse/error.h>
#include <GLFW/glfw3.h>

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <sstream>
#include <memory>
#include <stdlib.h>
#include <string.h>

// Dear ImGui
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"


// ============================================================
// HELPERS & DATA STRUCTURES
// ============================================================

struct AudioDevice {
    std::string internalName;
    std::string description;
    bool selectedAsDestination = false;
};

struct OutputDevice {
    std::string name;
    pa_simple* pRender = nullptr;
    std::atomic<bool> enabled{true};
    std::atomic<float> volume{0.5f};

    OutputDevice() = default;

    OutputDevice(std::string n, pa_simple* r = nullptr)
        : name(n), pRender(r), enabled(true), volume(0.5f) {}

    OutputDevice(const OutputDevice& other) {
        name = other.name;
        pRender = other.pRender;
        enabled.store(other.enabled.load());
        volume.store(other.volume.load());
    }

    OutputDevice(OutputDevice&& other) noexcept {
        name = std::move(other.name);
        pRender = other.pRender;
        enabled.store(other.enabled.load());
        volume.store(other.volume.load());
        other.pRender = nullptr;
    }
};

std::atomic<bool> g_IsRunning{false};

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

std::vector<AudioDevice> GetAudioDevices(const std::string& type) {
    std::vector<AudioDevice> devices;
    std::string command = "pactl list " + type;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return devices;

    char buffer[512];
    AudioDevice current;
    bool hasName = false;

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line = Trim(buffer);
        if (line.rfind("Name:", 0) == 0) {
            if (hasName) {
                devices.push_back(current);
                current = AudioDevice();
            }
            current.internalName = Trim(line.substr(5));
            hasName = true;
        } else if (line.rfind("Description:", 0) == 0) {
            current.description = Trim(line.substr(12));
        }
    }
    if (hasName) devices.push_back(current);
    pclose(pipe);
    return devices;
}

// In PulseAudio, the 'monitor' source has the same name as the sink, plus '.monitor'.
void MutePulseDevice(const std::string& monitorName, bool mute) {
    std::string sinkName = monitorName;
    size_t pos = sinkName.find(".monitor");
    if (pos != std::string::npos) {
        sinkName.erase(pos);
        std::string cmd = "pactl set-sink-mute " + sinkName + (mute ? " 1" : " 0");
        system(cmd.c_str());
    }
}

void DrawClickableLink(const char* displayText, const char* url) {
    ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "%s", displayText);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Open in browser:\n%s", url);
        if (ImGui::IsItemClicked()) {
            std::string cmd = std::string("xdg-open ") + url;
            system(cmd.c_str());
        }
    }
}

// ============================================================
// Assests - Icon as hex array
// Pallete mode = 32 bit RGBA (4bytes/pixel)
// ============================================================

static const int8_t soundicon_48[]  = {
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
  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000
};


// ============================================================
// AUDIO THREAD
// ============================================================

void AudioRoutingThread(pa_simple* pCapture, std::vector<OutputDevice>& destinations) {
    const int FRAMES = 1024;
    const int CHANNELS = 2;
    // We use Float32 to easily apply volume multipliers
    float buffer[FRAMES * CHANNELS]; 
    int error;

    while (g_IsRunning.load()) {
        if (pa_simple_read(pCapture, buffer, sizeof(buffer), &error) < 0) {
            break; // Read failed
        }

        for (auto& dest : destinations) {
            if (!dest.pRender) continue;

            if (dest.enabled.load()) {
                float currentVolume = dest.volume.load();
                float renderBuffer[FRAMES * CHANNELS];
                
                for (int i = 0; i < FRAMES * CHANNELS; i++) {
                    renderBuffer[i] = buffer[i] * currentVolume;
                }
                pa_simple_write(dest.pRender, renderBuffer, sizeof(renderBuffer), &error);
            } else {
                float silenceBuffer[FRAMES * CHANNELS] = {0};
                pa_simple_write(dest.pRender, silenceBuffer, sizeof(silenceBuffer), &error);
            }
        }
    }
}

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// ============================================================
// MAIN
// ============================================================

int main() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE); //maximised window

    GLFWwindow* window = glfwCreateWindow(1100, 720, "Echo Link", nullptr, nullptr);
    if (window == nullptr) return 1;

    GLFWimage images[1];
    images[0].width = 48;
    images[0].height = 48;
    
    images[0].pixels = (unsigned char*) soundicon_48;
    glfwSetWindowIcon(window, 1, images);


    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.IniFilename = nullptr; // Don't create imgui.ini

    ImGui::StyleColorsDark();

    // ========================================================
    // THEME
    // ========================================================
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 14.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding = 10.0f;
    style.TabRounding = 10.0f;
    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(10, 8);
    style.ItemSpacing = ImVec2(12, 12);

    ImVec4* colors = style.Colors;
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
    colors[ImGuiCol_SliderGrab] = ImVec4(0.25f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.35f, 0.85f, 1.00f, 1.00f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // ========================================================
    // ENUMERATE AUDIO DEVICES
    // ========================================================
    std::vector<AudioDevice> sources = GetAudioDevices("sources");
    std::vector<AudioDevice> sinks = GetAudioDevices("sinks");

    bool isStreaming = false;
    int selectedSourceIdx = 0;
    bool muteSourceDevice = true;
    std::string initializationError = "";

    std::vector<OutputDevice> activeStreamDevices;
    std::thread audioThread;
    pa_simple* pCaptureClient = nullptr;

    // PulseAudio Format (FLOAT32 for volume math)
    static const pa_sample_spec ss = {
        .format = PA_SAMPLE_FLOAT32LE,
        .rate = 44100,
        .channels = 2
    };

    // --- NEW: Custom Buffer Attributes for Low Latency ---
    // Calculate bytes for roughly 20ms of audio:
    // 44100 (rate) * 2 (channels) * 4 (bytes per Float32) = 352,800 bytes/sec
    // 352,800 * 0.020 seconds = 7056 bytes =~ 8192 (2^13).
    uint32_t latencyBytes = 8192; 

    pa_buffer_attr bufferAttr;
    bufferAttr.maxlength = (uint32_t)-1;      // Let the server decide max limit
    bufferAttr.tlength = latencyBytes;        // Playback target length
    bufferAttr.prebuf = (uint32_t)-1;         // Let the server decide pre-buffering
    bufferAttr.minreq = (uint32_t)-1;         // Let the server decide min request
    bufferAttr.fragsize = latencyBytes;       // Capture fragment size


    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(display_w, display_h));

        ImGui::Begin("EchoLink", nullptr, 
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

        // ====================================================
        // HEADER
        // ====================================================
        ImGui::TextColored(ImVec4(0.25f, 0.75f, 1.0f, 1.0f), "EchoLink");
        ImGui::SameLine();
        ImGui::TextDisabled("Real-Time Audio Router (Linux)");

        ImGui::SameLine(ImGui::GetWindowWidth() - 70.0f);
        if (ImGui::Button("Help")) {
            ImGui::OpenPopup("HelpMenu");
        }

        ImGui::Separator();

        if (ImGui::BeginPopupModal("HelpMenu", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("To stream silently to your Bluetooth devices on Linux:");
            ImGui::Spacing();
            ImGui::Text("Select a '.monitor' source in the dropdown.");
            ImGui::Text("Checking 'Mute Local Playback' will mute the corresponding local sink.");
            ImGui::Spacing();
            ImGui::TextDisabled("Made by VarunKumar0x10");
            ImGui::SameLine(); 
            DrawClickableLink("View GitHub Repository", "https://github.com/VarunKumar0x10/EchoLink");
            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::Spacing();

        // ====================================================
        // CONFIG UI
        // ====================================================
        if (!isStreaming) {
            ImGui::Text("1. Select Source Device");
            ImGui::SetNextItemWidth(500);

            std::string comboPreview = sources.empty() ? "None" : sources[selectedSourceIdx].description;
            if (ImGui::BeginCombo("##SourceCombo", comboPreview.c_str())) {
                for (size_t n = 0; n < sources.size(); n++) {
                    bool is_selected = (selectedSourceIdx == n);
                    if (ImGui::Selectable(sources[n].description.c_str(), is_selected)) {
                        selectedSourceIdx = n;
                    }
                    if (is_selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            ImGui::Checkbox("Mute Local Playback", &muteSourceDevice);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 
                "Action Required: Set this device as your Default Output in Linux sound settings.");
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("2. Select Destination Devices");
            ImGui::BeginChild("DestList", ImVec2(0, 350), true);
            for (size_t i = 0; i < sinks.size(); i++) {
                ImGui::Checkbox(sinks[i].description.c_str(), &sinks[i].selectedAsDestination);
            }
            ImGui::EndChild();
            ImGui::Spacing();

            // Check destinations
            bool hasDestination = false;
            for (const auto& sink : sinks) {
                if (sink.selectedAsDestination) { hasDestination = true; break; }
            }

            if (!hasDestination) ImGui::BeginDisabled(); 

            if (ImGui::Button("Start Audio Routing", ImVec2(-1, 50))) {
                initializationError = "";
                int error;

                if (muteSourceDevice && !sources.empty()) {
                    MutePulseDevice(sources[selectedSourceIdx].internalName, true);
                }

                pCaptureClient = pa_simple_new(NULL, "EchoLink", PA_STREAM_RECORD, 
        sources[selectedSourceIdx].internalName.c_str(), "Capture", &ss, NULL, &bufferAttr, &error);

                if (!pCaptureClient) {
                    initializationError = "Failed to initialize source client: " + std::string(pa_strerror(error));
                } else {
                    for (auto& sink : sinks) {
                        if (!sink.selectedAsDestination) continue;
                        
                        pa_simple* pRender = pa_simple_new(NULL, "EchoLink", PA_STREAM_PLAYBACK, 
        sink.internalName.c_str(), "Playback", &ss, NULL, &bufferAttr, &error);

                        if (pRender) {
                            activeStreamDevices.push_back(OutputDevice(sink.description, pRender));
                        }
                    }

                    if (activeStreamDevices.empty()) {
                        initializationError = "Failed to initialize any destinations.";
                        pa_simple_free(pCaptureClient);
                        pCaptureClient = nullptr;
                    } else {
                        g_IsRunning.store(true);
                        audioThread = std::thread(AudioRoutingThread, pCaptureClient, std::ref(activeStreamDevices));
                        isStreaming = true;
                    }
                }
            }

            if (!hasDestination) {
                ImGui::EndDisabled();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "(!) Please select at least one destination device to begin.");
            }

            if (!initializationError.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "Error: %s", initializationError.c_str());
            }
        } 
        // ====================================================
        // STREAMING UI
        // ====================================================
        else {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "LIVE");
            ImGui::SameLine();
            ImGui::Text("Streaming audio from:");
            ImGui::TextWrapped("%s", sources[selectedSourceIdx].description.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::BeginChild("OutputsPanel", ImVec2(0, -80), false);
            for (size_t i = 0; i < activeStreamDevices.size(); i++) {
                ImGui::PushID((int)i);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
                ImGui::BeginChild(("DeviceCard" + std::to_string(i)).c_str(), ImVec2(0, 160), true, 
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "%s", activeStreamDevices[i].name.c_str());

                bool isEnabled = activeStreamDevices[i].enabled.load();
                if (ImGui::Checkbox("Enabled", &isEnabled)) {
                    activeStreamDevices[i].enabled.store(isEnabled);
                }

                float volPercent = activeStreamDevices[i].volume.load() * 100.0f;
                ImGui::Text("Volume");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##VolumeSlider", &volPercent, 0.0f, 100.0f, "%.0f%%")) {
                    activeStreamDevices[i].volume.store(volPercent / 100.0f);
                }

                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::Spacing();
                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::Spacing();
            if (ImGui::Button("Stop Streaming", ImVec2(-1, 50))) {
                g_IsRunning.store(false);
                if (audioThread.joinable()) { audioThread.join(); }

                if (pCaptureClient) {
                    pa_simple_free(pCaptureClient);
                    pCaptureClient = nullptr;
                }
                for (auto& dev : activeStreamDevices) {
                    if (dev.pRender) pa_simple_free(dev.pRender);
                }
                activeStreamDevices.clear();
                
                // Unmute Source 
                if (muteSourceDevice) {
                    MutePulseDevice(sources[selectedSourceIdx].internalName, false);
                }
                
                // Refresh devices upon stop in case topology changed
                sources = GetAudioDevices("sources");
                sinks = GetAudioDevices("sinks");
                
                isStreaming = false;
            }
        }

        ImGui::End();
        ImGui::Render();
        
        const float clear_color_with_alpha[4] = {0.06f, 0.07f, 0.09f, 1.0f};
        glClearColor(clear_color_with_alpha[0], clear_color_with_alpha[1], clear_color_with_alpha[2], clear_color_with_alpha[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Cleanup
    if (isStreaming) {
        g_IsRunning.store(false);
        if (audioThread.joinable()) audioThread.join();
        if (pCaptureClient) pa_simple_free(pCaptureClient);
        for (auto& dev : activeStreamDevices) {
            if (dev.pRender) pa_simple_free(dev.pRender);
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
        
