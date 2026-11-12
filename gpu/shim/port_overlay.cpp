// SPDX-License-Identifier: GPL-2.0-or-later
#include "port_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include "port_settings.h"
#include "port_ui_language.h"
#include "port_graphics.h"
#include "port_perf_stats.h"
#include "../theordergpu.h"

extern "C" void runtime_restart(void); // src/probe.c: starts run.py again, ends this process
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global port_font_ttf\n"
    "port_font_ttf:\n"
    ".incbin \"" THEORDER_FONT_PATH "\"\n"
    ".global port_font_ttf_end\n"
    "port_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden port_font_ttf\n"
    ".global port_font_ttf\n"
    "port_font_ttf:\n"
    ".incbin \"" THEORDER_FONT_PATH "\"\n"
    ".hidden port_font_ttf_end\n"
    ".global port_font_ttf_end\n"
    "port_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char port_font_ttf[];
extern "C" const unsigned char port_font_ttf_end[];

namespace PortOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
std::atomic<bool> exit_requested{false}; // Game tab > Exit to Desktop, confirmed
bool r3_down = false, l2_down = false;
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
bool dirty = false; // settings changed while open: saved on close
int requested_tab = -1; // switch on the next frame (L1/R1, Q/E, menu opened)
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<PortPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void ClearInput() {
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    ImGui::ClearActiveID();
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ClearInput();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = value;
    if (value) {
        requested_tab = PortSettings::Get().menu_tab;
    }
    if (!value && dirty) {
        dirty = false;
        PortSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

using PortUi::Tr;

// The language in use: the setting, or Windows' display language when it is automatic.
int SystemLanguage();
void ApplyLanguage() {
    const int setting = PortSettings::Get().ui_language;
    PortUi::SetLanguage(setting >= 0 ? setting : SystemLanguage());
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

// A combo over `count` labels storing an index or a value from `values`.
void Choice(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
            int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    // The labels in the menu's language (the values stay the same).
    std::vector<const char*> shown(labels, labels + count);
    for (const char*& text : shown) {
        text = Tr(text);
    }
    if (ImGui::Combo(label, &current, shown.data(), count)) {
        Store(target, values ? values[current] : current, true);
    }
}

// Controller button names in the help text: dynamically adapts to the current input
// (Xbox, PlayStation or Keyboard & Mouse) or falls back to THEORDER_GLYPHS.
enum class ActiveInput {
    Default,
    PlayStation,
    Xbox,
    Keyboard,
};
static ActiveInput last_active_input = ActiveInput::Default;

ActiveInput GamepadInput(SDL_JoystickID device) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(device);
    const SDL_GamepadType type = pad ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_UNKNOWN;
    return type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5
               ? ActiveInput::PlayStation : ActiveInput::Xbox;
}

void SelectInput(ActiveInput input) {
    if (input == ActiveInput::Keyboard && last_active_input != input) {
        ImGuiIO& io = ImGui::GetIO();
        for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key) {
            io.AddKeyAnalogEvent(static_cast<ImGuiKey>(key), false, 0.0f);
        }
        if (ImGui::GetCurrentContext()->ActiveIdSource == ImGuiInputSource_Gamepad) {
            ImGui::ClearActiveID();
        }
    }
    last_active_input = input;
}

static const char* PadText(const char* playstation, const char* xbox, const char* nintendo, const char* kbm = nullptr) {
    if (last_active_input == ActiveInput::Keyboard && kbm) {
        return kbm;
    }
    if (last_active_input == ActiveInput::Xbox) {
        return xbox;
    }
    if (last_active_input == ActiveInput::PlayStation) {
        return playstation;
    }
    const char* style = std::getenv("THEORDER_GLYPHS");
    if (style) {
        if (!std::strcmp(style, "xbox")) return xbox;
        if (!std::strcmp(style, "nintendo")) return nintendo;
        if (!std::strcmp(style, "keyboard") && kbm) return kbm;
    }
    return playstation;
}

// The note for the item under the mouse or the gamepad/keyboard focus, shown in the footer.
const char* footer_help = nullptr;

void Help(const char* text) {
    if (text && (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())) {
        footer_help = Tr(text);
    }
}

// Two-column rows: the name on the left, the control on the right.
bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr(label));
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help = nullptr) {
    Row(label);
    Checkbox("##value", value);
    Help(help);
    ImGui::PopID();
}

void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help = nullptr) {
    Row(label);
    Choice("##value", target, labels, values, count);
    Help(help);
    ImGui::PopID();
}

void InfoRow(const char* label, const char* text) {
    Row(label);
    ImGui::TextDisabled("%s", Tr(text));
    ImGui::PopID();
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help) {
    Row(label);
    int v = value;
    if (ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case PortSettings::UpscalerFsr3: return "FSR 3.1";
    case PortSettings::UpscalerFsr4: return "FSR 4";
    case PortSettings::UpscalerFsr411: return "FSR 4.1.1";
    case PortSettings::UpscalerTaa: return "TAA";
    case PortSettings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = PortPerfStats;
    const auto& s = PortSettings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance);
    }
    P::Sample sample = P::Latest();
    sample.fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    sample.frame_ms = frame_ms_avg;
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0, s.overlay_advanced);
}

void ImageTab() {
    namespace G = PortGraphics;
    auto& s = PortSettings::Get();
    ImGui::SeparatorText(Tr("Display"));
    if (BeginRows("##display")) {
        ChoiceRow("Display mode", s.display_mode, G::DisplayModeNames.data(), nullptr, G::DisplayModeCount,
                  "Windowed, borderless window or exclusive fullscreen.");
        CheckRow("VSync", s.vsync, "Waits for the monitor: no tearing, a little more input delay.");
        SliderRow("Field of view", s.fov, 50, 200, "%d%%",
                  "How much of the scene the camera shows. Above 100% shows more on every side.");
        PortSettings::ApplyFov();
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Tr("Mouse Look"));
    if (BeginRows("##mouse")) {
        CheckRow("Mouse look (F7)", s.mouse_look,
                 "The mouse turns the camera; left button fires (R2), right button aims (L2), "
                 "middle R3, side buttons L1/R1.");
        ImGui::BeginDisabled(!s.mouse_look);
        SliderRow("Horizontal sensitivity (X)", s.mouse_sensitivity, 10, 400, "%d%%",
                  "How fast the camera turns horizontally with the mouse.");
        SliderRow("Vertical sensitivity (Y)", s.mouse_sensitivity_y, 10, 400, "%d%%",
                  "How fast the camera turns vertically with the mouse.");
        CheckRow("Invert Y-axis", s.mouse_invert_y, "Invert vertical camera look direction.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Tr("Resolution & Upscaling"));
    if (BeginRows("##upscaling")) {
        static const char* const res_labels[] = {
            "1280 x 720 (720p)", "1600 x 900 (900p)", "1920 x 1080 (1080p)", "2560 x 1440 (1440p)",
            "3440 x 1440 (21:9 Ultrawide)", "3840 x 1080 (32:9 Ultrawide)",
            "3840 x 2160 (4K)", "5120 x 1440 (32:9 Super Ultrawide)"
        };
        static const int res_values[] = {0, 5, 1, 2, 6, 7, 3, 4};
        ChoiceRow("Output resolution", s.output_res, res_labels, res_values, PortSettings::OutputCount,
                  "Output render and presentation target size.");

        static const char* const upscaler_labels[] = {
            "Off (Native)", "FSR 3.1", "FSR 4", "FSR 4.1.1", "TAA", "DLSS"
        };
        static const int upscaler_values[] = {
            PortSettings::UpscalerOff, PortSettings::UpscalerFsr3, PortSettings::UpscalerFsr4,
            PortSettings::UpscalerFsr411, PortSettings::UpscalerTaa, PortSettings::UpscalerDlss
        };
        ChoiceRow("Upscaler", s.upscaler, upscaler_labels, upscaler_values, 6,
                  "Temporal upscaler (Off, FSR 3.1, FSR 4, FSR 4.1.1, TAA, DLSS).");
        if (s.upscaler == PortSettings::UpscalerDlss && !s.dlss_supported) {
            InfoRow("DLSS Status", s.dlss_problem ? s.dlss_problem.load() : "DLSS not available on this GPU or DLL missing.");
        }
        if (PortSettings::IsFsr4(s.upscaler) && !s.fsr4_supported) {
            InfoRow("FSR 4 Status", s.fsr4_problem ? s.fsr4_problem.load() : "FSR 4 not supported on this GPU.");
        }

        ImGui::BeginDisabled(s.upscaler == PortSettings::UpscalerOff);
        static const char* const preset_labels[] = {
            "Native AA (1.0x)", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2.0x)", "Ultra Performance (3.0x)"
        };
        ChoiceRow("Quality preset", s.preset, preset_labels, nullptr, 5,
                  "Render scale per axis for the selected upscaler.");
        ImGui::EndDisabled();

        ImGui::EndTable();
    }
    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    const bool downscaling = iw > ww || ih > wh;
    const bool rcas_blocked = s.rcas && !s.rcas_applied;
    ImGui::SeparatorText(Tr("Applied on restart"));
    if (BeginRows("##image_restart")) {
        ChoiceRow("Aspect ratio", s.aspect, G::AspectNames.data(), nullptr, int(G::AspectNames.size()),
                  "The shape of the game's picture. Pick your screen's to fill it without black bars: "
                  "the view keeps the original width and shows more above and below.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Tr("Sharpening"));
    if (BeginRows("##scaling")) {
        ImGui::BeginDisabled(rcas_blocked);
        CheckRow("Sharpening (RCAS)", s.rcas,
                 rcas_blocked && downscaling ? "No effect: the game's image is larger than the window."
                 : rcas_blocked              ? "No effect: the game's image is smaller than the window."
                                             : "Sharpens the final image.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.rcas || rcas_blocked);
        SliderRow("Sharpening strength", s.rcas_strength, 0, 100, "%d%%", "How strong the sharpening is.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Tr("Information"));
    if (BeginRows("##image_info")) {
        char text[96];
        const int out_idx = std::clamp(s.output_res.load(), 0, PortSettings::OutputCount - 1);
        std::snprintf(text, sizeof(text), "%d x %d", iw, ih);
        InfoRow("Render resolution", text);
        std::snprintf(text, sizeof(text), "%d x %d",
                      PortSettings::OutputWidths[out_idx], PortSettings::OutputHeights[out_idx]);
        InfoRow("Target output", text);
        std::snprintf(text, sizeof(text), "%d x %d", ww, wh);
        InfoRow("Window size", text);
        InfoRow("Active upscaler", UpscalerLabel(s.upscaler));
        InfoRow("Sharpening", s.rcas_applied ? "applied" : "not applied");
        ImGui::EndTable();
    }
}

void PerformanceTab() {
    namespace G = PortGraphics;
    auto& s = PortSettings::Get();
    ImGui::SeparatorText(Tr("Live"));
    if (BeginRows("##live")) {
        static const char* const fps_labels[] = {"30", "60", "120", "240", "Unlimited"};
        ChoiceRow("Frame rate limit", s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  "Caps the frame rate. The engine frame rate (below) is the highest it can go.");
        // The Order presents every other vblank of its engine rate: 30 FPS at "60", 60 with the patch.
        const int effective = G::EffectiveFps(s.fps_limit, s.applied_engine_fps / 2);
        if (s.fps_limit == 0 || effective < s.fps_limit) {
            char text[96];
            std::snprintf(text, sizeof(text), Tr("%d FPS (engine frame rate)"), effective);
            InfoRow("Effective limit", text);
        }
        if (s.applied_engine_fps < s.startup_engine_fps) {
            char text[192];
            std::snprintf(text, sizeof(text), Tr("%d FPS: the %d FPS patch is not available for this game build"),
                          s.applied_engine_fps / 2, s.startup_engine_fps / 2);
            InfoRow("Engine frame rate", text);
        }
        static const char* const queued_labels[] = {"1 (lowest latency)", "2", "Unlimited"};
        ChoiceRow("Frames queued", s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), "More frames queued: steadier FPS, slower response to input.");
        CheckRow("Compile new shaders in the background", s.async_shaders,
                 "Fewer stutters in new areas; an object or effect may appear a moment later.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Tr("Applied on restart"));
    if (BeginRows("##restart")) {
        // Engine 60 is The Order's own 30 FPS; 120 asks for its 60 FPS patch, 240 for 120 FPS.
        static const char* const engine_labels[] = {"30 (original)", "60 (experimental)", "120 (experimental)"};
        ChoiceRow("Engine frame rate", s.engine_fps, engine_labels, G::EngineFps.data(), int(G::EngineFps.size()),
                  "The highest frame rate the game itself runs at (a game patch).");
        CheckRow("Game reads GPU data without waiting", s.stale_readback,
                 "Big FPS gain. Switch it off if lighting or objects look wrong.");
        CheckRow("Deferred GPU readbacks", s.deferred_readback,
                 "Fewer waits for the GPU. Switch it off if textures or shadows look wrong.");
        // port (SpartanPort 8192bbb): a generated frame between every two real ones.
        static const char* const fg_labels[] = {"Off", "AMD FSR 3.1 (2x, experimental)"};
        ChoiceRow("Frame generation", s.frame_generation, fg_labels, nullptr, 2,
                  "Experimental. Shows a generated frame between every two real ones: twice the frames "
                  "on screen, slightly more input delay. Needs the upscaler at a scaled preset; on this "
                  "game build it does not run yet (issue #138), so this only adds latency.");
        if (s.startup_frame_generation != 0) {
            const char* status = s.fg_status.load();
            InfoRow("Frame generation status", status ? status : "waiting for an upscaled frame");
        }
        ImGui::EndTable();
    }
}

void OverlayTab() {
    namespace P = PortPerfStats;
    auto& s = PortSettings::Get();
    if (BeginRows("##overlay_on")) {
        CheckRow("Show overlay", s.show_fps, "The performance overlay in a corner of the screen.");
        CheckRow("Advanced metrics", s.overlay_advanced,
                 "Separate private commit, working set, Vulkan allocations and adapter totals.");
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!s.show_fps);
    ImGui::SeparatorText(Tr("What to show"));
    if (ImGui::BeginTable("##overlay_items", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        static const char* const names[] = {"FPS", "Frametime", "Upscaler", "GPU (name and use)", "CPU (game)",
                                            "RAM (private commit)", "GPU memory (adapter / budget)"};
        for (unsigned item = 0; item < 7; ++item) {
            const unsigned bit = 1u << item;
            bool on = (s.overlay_items & bit) != 0;
            const std::string label = std::string(Tr(names[item])) + "###" + names[item];
            if (ImGui::Checkbox(label.c_str(), &on)) {
                s.overlay_items = on ? (s.overlay_items | bit) : (s.overlay_items & ~bit);
                dirty = true;
            }
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", Tr("Preview"));
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
        const std::string text = OverlayText();
        ImGui::TextUnformatted(text.empty() ? Tr("(nothing selected)") : text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Tr("Appearance"));
    if (BeginRows("##appearance")) {
        SliderRow("Font size", s.overlay_scale, 50, 300, "%d%%", "Size of the overlay text.");
        static const char* const corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        ChoiceRow("Corner", s.overlay_corner, corners, nullptr, 4, "Where the overlay sits.");
        SliderRow("Background opacity", s.overlay_opacity, 0, 100, "%d%%", "0% is no background.");
        static const char* const layouts[] = {"One line", "One item per line"};
        ChoiceRow("Layout", s.overlay_layout, layouts, nullptr, 2, "Items side by side or stacked.");
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText(Tr("About"));
    if (BeginRows("##about")) {
        const auto& elf = Common::ElfInfo::Instance();
        char text[160];
        std::snprintf(text, sizeof(text), "%.*s v%.*s", int(elf.GameSerial().size()), elf.GameSerial().data(),
                      int(elf.AppVer().size()), elf.AppVer().data());
        InfoRow("Game", text);
        const u32 total = background_total;
        if (total) {
            std::snprintf(text, sizeof(text), Tr("loading %u%%"), u32(u64(background_done) * 100 / total));
        } else {
            std::snprintf(text, sizeof(text), "%s", Tr("ready"));
        }
        InfoRow("Shader cache", text);
        ImGui::EndTable();
    }
}

void GameTab() {
    auto& s = PortSettings::Get();
    ImGui::SeparatorText(Tr("Game"));
    if (BeginRows("##game")) {
        CheckRow("Pause the game", s.pause_game, "The game stops while this menu is open.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Tr("Audio"));
    if (BeginRows("##audio")) {
        SliderRow("Master volume", s.master_volume, PortSettings::MasterVolumeMin, PortSettings::MasterVolumeMax,
                  "%d%%", "100% is the game's own mix; more is boosted behind a soft limiter.");
        static const char* const speaker_labels[] = {"Automatic (the device's)", "Stereo", "5.1", "7.1"};
        static const int speaker_values[] = {PortSettings::AudioChannelsAuto, PortSettings::AudioStereo,
                                             PortSettings::Audio51, PortSettings::Audio71};
        ChoiceRow("Speakers (applied on restart)", s.audio_channels, speaker_labels, speaker_values, 4,
                  "Layout of the game's 7.1 sound. Automatic follows the playback device.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Tr("Language"));
    if (BeginRows("##language")) {
        // The names stay in their own language (they are not table keys).
        static std::vector<const char*> language_labels;
        static std::vector<int> language_values;
        if (language_labels.empty()) {
            language_labels.push_back("Automatic (Windows)");
            language_values.push_back(-1);
            for (int i = 0; i < PortUi::LanguageCount; ++i) {
                language_labels.push_back(PortUi::Languages[i].name);
                language_values.push_back(i);
            }
        }
        const int before = s.ui_language;
        ChoiceRow("Menu language", s.ui_language, language_labels.data(), language_values.data(),
                  int(language_labels.size()), "The language of this menu. Automatic follows Windows.");
        if (s.ui_language != before) {
            ApplyLanguage();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText(Tr("Session"));
    if (BeginRows("##session")) {
        // In the value column like every other row, so the d-pad and arrow keys reach it from the
        // rows above (a button in the label column was skipped on the way to Close).
        Row("Leave the game");
        const std::string exit_label = std::string(Tr("Exit to Desktop")) + "###exit_to_desktop";
        if (ImGui::Button(exit_label.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
            ImGui::OpenPopup("###exit_question");
        }
        Help("Closes the game, as closing the window does. Progress since the last save is lost.");
        const std::string question = std::string(Tr("Exit to Desktop?")) + "###exit_question";
        if (ImGui::BeginPopupModal(question.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(Tr("Progress since the last save is lost."));
            if (ImGui::Button(Tr("Exit"))) {
                // Leaves at the start of the next Render, outside imgui_mutex: the window thread
                // keeps handling events while the shader cache drains.
                exit_requested = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::EndTable();
    }
}

void ControlsTab() {
    auto& s = PortSettings::Get();
    ImGui::SeparatorText(Tr("Active Input"));
    if (BeginRows("##active_input")) {
        const char* current_type = "PlayStation Controller";
        if (last_active_input == ActiveInput::Keyboard) {
            current_type = "Keyboard & Mouse";
        } else if (last_active_input == ActiveInput::Xbox) {
            current_type = "Xbox Controller";
        }
        InfoRow("Detected device", current_type);
        ImGui::EndTable();
    }

    ImGui::SeparatorText(Tr("Mouse Look"));
    if (BeginRows("##mouse_settings")) {
        CheckRow("Mouse look (F7)", s.mouse_look, "Use mouse movement to aim and look around.");
        SliderRow("Horizontal sensitivity (X)", s.mouse_sensitivity, 10, 400, "%d%%", "Horizontal look sensitivity percentage.");
        SliderRow("Vertical sensitivity (Y)", s.mouse_sensitivity_y, 10, 400, "%d%%", "Vertical look sensitivity percentage.");
        CheckRow("Invert Y-axis", s.mouse_invert_y, "Invert vertical look direction.");
        ImGui::EndTable();
    }

    ImGui::SeparatorText(Tr("Default Controls Reference"));
    if (BeginRows("##controls_ref")) {
        InfoRow("Shoot / Fire", PadText("R2", "RT", "ZR", "Left Mouse Button"));
        InfoRow("Aim Weapon", PadText("L2", "LT", "ZL", "Right Mouse Button"));
        InfoRow("Interact / Reload", PadText("Square", "X", "Y", "E"));
        InfoRow("Jump / Climb / Cover", PadText("Cross", "A", "B", "Space"));
        InfoRow("Crouch / Drop", PadText("Circle", "B", "A", "Left Shift"));
        InfoRow("Melee Attack", PadText("Triangle", "Y", "X", "Q"));
        InfoRow("Blackwater / Heal", PadText("L1", "LB", "L", "1 / Side Mouse 1"));
        InfoRow("Throw Grenade", PadText("R1", "RB", "R", "3 / Side Mouse 2"));
        InfoRow("Sprint", PadText("L3 (Left Stick Click)", "LS", "LS", "Z"));
        InfoRow("Zoom / Precision Aim", PadText("R3 (Right Stick Click)", "RS", "RS", "C / Middle Click"));
        InfoRow("Pause Menu", PadText("Options / Touchpad Right", "Menu / View", "+ / -", "Backspace / Esc"));
        InfoRow("Photo Mode", PadText("Touchpad Left", "View (Select)", "-", "Tab"));
        ImGui::EndTable();
    }
}

// Startup options changed since this launch (applied by "Apply and restart").
int RestartChanges() {
    const auto& s = PortSettings::Get();
    return (s.aspect != s.startup_aspect) + (s.engine_fps != s.startup_engine_fps) +
           (s.deferred_readback != s.startup_deferred_readback) + (s.stale_readback != s.startup_stale_readback) +
           (s.frame_generation != s.startup_frame_generation);
}

void Menu() {
    auto& s = PortSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f * base_scale, viewport->WorkSize.x * 0.95f),
                                    std::min(680.0f * base_scale, viewport->WorkSize.y * 0.9f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    // port: the game's own title (param.sfo), dynamically formatted with active input prompts.
    const std::string_view title = Common::ElfInfo::Instance().Title();
    const std::string heading = std::string(title.empty() ? "The Order: 1886" : title) +
                                PadText("  (Insert / R3+L2)", "  (Insert / RS+LT)", "  (Insert / RS+ZL)", "  (Insert)") +
                                "###port_settings_menu";
    if (!ImGui::Begin(heading.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    footer_help = nullptr;
    constexpr int TabCount = PortSettings::MenuTabCount;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        const int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
                         (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) ||
                         ImGui::IsKeyPressed(ImGuiKey_PageDown, false)
                             ? 1
                         : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ||
                           (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) ||
                           ImGui::IsKeyPressed(ImGuiKey_PageUp, false)
                             ? -1
                             : 0;
        if (step) {
            requested_tab = (s.menu_tab + step + TabCount) % TabCount;
        }
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTabBar("##tabs")) {
        static const char* const tabs[TabCount] = {"Image", "Performance", "Overlay", "Controls", "Game"};
        for (int tab = 0; tab < TabCount; ++tab) {
            const ImGuiTabItemFlags flags = requested_tab == tab ? ImGuiTabItemFlags_SetSelected : 0;
            const std::string tab_label = std::string(Tr(tabs[tab])) + "###" + tabs[tab];
            if (!ImGui::BeginTabItem(tab_label.c_str(), nullptr, flags)) {
                continue;
            }
            if (s.menu_tab != tab && requested_tab < 0) {
                s.menu_tab = tab;
                dirty = true;
            }
            // NavFlattened: the d-pad and arrow keys go from the tab bar into the rows and on to Close;
            // without it they skipped the whole body (a separate navigation window).
            if (ImGui::BeginChild("##body", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened)) {
                const float scroll = ImGui::GetKeyMagnitude2d(ImGuiKey_GamepadRStickLeft,
                    ImGuiKey_GamepadRStickRight, ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown).y;
                if (last_active_input != ActiveInput::Keyboard && scroll != 0.0f &&
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                    ImGui::SetScrollY(ImGui::GetScrollY() +
                        scroll * ImGui::GetTextLineHeightWithSpacing() * 20.0f * io.DeltaTime);
                }
                switch (tab) {
                case 0: ImageTab(); break;
                case 1: PerformanceTab(); break;
                case 2: OverlayTab(); break;
                case 3: ControlsTab(); break;
                default: GameTab(); break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (requested_tab >= 0) {
            s.menu_tab = requested_tab;
            dirty = true;
            requested_tab = -1;
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", footer_help ? footer_help : Tr(PadText("L1 / R1: switch tabs. Settings are saved in theorder.ini.", "LB / RB: switch tabs. Settings are saved in theorder.ini.", "L / R: switch tabs. Settings are saved in theorder.ini.", "Ctrl+Tab or PageUp/Down: switch tabs. Settings are saved in theorder.ini.")));
    ImGui::PopTextWrapPos();
    if (const int changes = RestartChanges()) {
        // One text per number: the English plural trick does not translate.
        if (changes == 1) {
            ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s",
                               Tr("1 change needs a restart (unsaved progress is lost)."));
        } else {
            ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f),
                               Tr("%d changes need a restart (unsaved progress is lost)."), changes);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!std::getenv("THEORDER_RESTART_COMMAND"));
        if (ImGui::Button(Tr("Apply and restart"))) {
            // The new launch confirms the options once the game shows; run.py restores the
            // previous ones if it never does (restart_unconfirmed).
            s.restart_unconfirmed = true;
            PortSettings::Save();
            runtime_restart();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::NewLine();
    }
    const float close_width = ImGui::CalcTextSize(Tr("Close")).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_width);
    if (ImGui::Button(Tr("Close"))) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("%s", Tr("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel"));
    ImGui::TextDisabled("%s", Tr(PadText("Controller: Cross (A) = OK, Circle (B) = cancel", "Controller: A = OK, B = cancel", "Controller: B = OK, A = cancel")));
    ImGui::End();
}

void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.36f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", choice_title.c_str());
    ImGui::Separator();
    // Save pictures are 228x128; rows without one keep the text aligned when others have one.
    const bool pictures = std::any_of(choice_textures.begin(), choice_textures.end(),
                                      [](ImTextureData* tex) { return tex != nullptr; });
    const ImVec2 picture_size(228.0f * base_scale, 128.0f * base_scale);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ImGui::Image(tex->GetTexRef(), picture_size);
        } else if (pictures) {
            ImGui::Dummy(picture_size);
        }
        if (pictures) {
            ImGui::SameLine();
        }
        if (selected) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "> %s", choice_items[i].c_str());
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::EndGroup();
        if (selected) {
            if (pictures) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float pad = 3.0f * base_scale;
                ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - pad, min.y - pad),
                                                    ImVec2(max.x + pad, max.y + pad),
                                                    IM_COL32(255, 230, 140, 255), 0.0f, 0,
                                                    2.0f * base_scale);
            }
            ImGui::SetScrollHereY();
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", Tr(PadText("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel", "Up/Down or D-pad: choose   Enter or A: OK   Esc or B: cancel", "Up/Down or D-pad: choose   Enter or B: OK   Esc or A: cancel")));
    ImGui::End();
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(Tr(total ? "Loading shader cache..." : "Preparing graphics..."));
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("%s", Tr("The first start after an update takes longer."));
    ImGui::End();
}

void FpsCounter() {
    const auto& s = PortSettings::Get();
    const std::string text = OverlayText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(s.overlay_opacity / 100.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled(Tr("Loading shader cache %u%%"), u32(u64(done) * 100 / total));
    ImGui::End();
}

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned short __stdcall GetUserDefaultUILanguage(void);
#endif

// Windows' display language (its primary language id), or LANG elsewhere, as a menu language.
int SystemLanguage() {
#ifdef _WIN32
    switch (GetUserDefaultUILanguage() & 0x3ff) {
    case 0x16: return PortUi::FindLanguage("pt");
    case 0x0a: return PortUi::FindLanguage("es");
    case 0x0c: return PortUi::FindLanguage("fr");
    case 0x07: return PortUi::FindLanguage("de");
    case 0x10: return PortUi::FindLanguage("it");
    default: return 0;
    }
#else
    const char* lang = std::getenv("LANG");
    return lang ? PortUi::FindLanguage(lang) : 0;
#endif
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    ApplyLanguage();
    std::printf("Overlay: menu language %s\n", PortUi::Languages[PortUi::language.load()].name);
    if (const char* log = std::getenv("THEORDER_PERF_LOG"); log && std::strcmp(log, "1") == 0) {
        PortPerfStats::Start(instance);
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "port";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(port_font_ttf),
                                   int(port_font_ttf_end - port_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: menu ready (Insert or R3+L2)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const PortPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        r3_down = l2_down = false;
        ClearInput();
        ImGui::GetIO().AddFocusEvent(false);
        return false;
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        ImGui::GetIO().AddFocusEvent(true);
        return false;
    }
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down) {
            SelectInput(ActiveInput::Keyboard);
        }
        static const bool capture_key = [] {
            const char* env = std::getenv("THEORDER_FRAME_CAPTURE_KEY");
            return env && *env == '1';
        }();
        if (down && !event.key.repeat && capture_key && event.key.key == SDLK_F11) {
            Vulkan::FrameCapture::Request();
            return true;
        }
        if (down && !event.key.repeat && event.key.key == SDLK_INSERT) {
            SetOpen(!is_open);
            return true;
        }
        if (down && !event.key.repeat && is_open && event.key.key == SDLK_ESCAPE) {
            if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                io.AddKeyEvent(ImGuiKey_Escape, true);
                return true;
            }
            SetOpen(false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (down) {
            SelectInput(GamepadInput(event.gbutton.which));
        }
        const u8 button = event.gbutton.button;
        // R3+L2 opens the menu: L3+R3 is left to the game.
        if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
            if (down && l2_down) {
                SetOpen(!is_open);
                return true;
            }
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const bool trigger = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                             event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if (std::abs(static_cast<int>(event.gaxis.value)) > (trigger ? 16000 : 8000)) {
            SelectInput(GamepadInput(event.gaxis.which));
        }
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            const bool down = event.gaxis.value > 16000;
            if (down && !l2_down && r3_down) {
                l2_down = true;
                SetOpen(!is_open);
                return true;
            }
            l2_down = down;
        }
        if (!is_open) {
            return false;
        }
        ImGuiKey negative = ImGuiKey_None, positive = ImGuiKey_None;
        switch (event.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            negative = ImGuiKey_GamepadLStickLeft; positive = ImGuiKey_GamepadLStickRight; break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            negative = ImGuiKey_GamepadLStickUp; positive = ImGuiKey_GamepadLStickDown; break;
        case SDL_GAMEPAD_AXIS_RIGHTX:
            negative = ImGuiKey_GamepadRStickLeft; positive = ImGuiKey_GamepadRStickRight; break;
        case SDL_GAMEPAD_AXIS_RIGHTY:
            negative = ImGuiKey_GamepadRStickUp; positive = ImGuiKey_GamepadRStickDown; break;
        default: return true;
        }
        const float value = static_cast<float>(event.gaxis.value);
        const float low = std::clamp((-value - 8000.0f) / (32768.0f - 8000.0f), 0.0f, 1.0f);
        const float high = std::clamp((value - 8000.0f) / (32767.0f - 8000.0f), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(negative, low > 0.0f, low);
        io.AddKeyAnalogEvent(positive, high > 0.0f, high);
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (event.motion.xrel != 0.0f || event.motion.yrel != 0.0f) {
            SelectInput(ActiveInput::Keyboard);
        }
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            SelectInput(ActiveInput::Keyboard);
        }
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (event.wheel.x != 0.0f || event.wheel.y != 0.0f) {
            SelectInput(ActiveInput::Keyboard);
        }
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.wheel.windowID);
        io.AddMousePosEvent(event.wheel.mouse_x * density, event.wheel.mouse_y * density);
        const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        io.AddMouseWheelEvent(event.wheel.x * direction, event.wheel.y * direction);
        return true;
    }
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = PortSettings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    PortSettings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

bool Visible() {
    const auto& s = PortSettings::Get();
    return initialized && (loading || background_total || menu_open || text_entry_active || choice_active ||
                           choice_retiring || s.show_fps);
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active;
}

bool GamePaused() {
    return PortSettings::Get().pause_game && menu_open && !loading;
}

void WaitWhilePaused(const char* who) {
    // The game's own thread stops here, between two frames: its clock goes on, and the game takes
    // the gap as one long frame, as after a load.
    if (!GamePaused()) {
        return;
    }
    // One counter per caller kind (frames, sound): the first pauses of each are logged.
    static std::atomic<unsigned> logged_frame{0}, logged_sound{0};
    auto& logged = who && who[0] == 's' ? logged_sound : logged_frame;
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
        std::printf("Overlay: game paused while the menu is open (%s)\n", who ? who : "frame");
    }
    while (GamePaused()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<PortPng::Image> pictures, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    ConfirmRestart();
    if (exit_requested) {
        PortSettings::Save();
        theordergpu_exit_to_desktop();
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    if (menu_open) {
        Menu();
    }
    if (PortSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active && !menu_open) {
        ChoiceBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace PortOverlay
