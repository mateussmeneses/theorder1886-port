// SPDX-License-Identifier: GPL-2.0-or-later
#include "port_settings.h"
#include "port_ui_language.h"
#include "port_graphics.h"

#include <cctype>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace PortSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("THEORDER_CONFIG");
    return env && env[0] ? env : "theorder.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    static const char* const overlay_keys[] = {"overlay_fps", "overlay_frametime", "overlay_upscaler",
                                               "overlay_gpu", "overlay_cpu", "overlay_ram", "overlay_vram"};
    for (unsigned item = 0; item < 7; ++item) {
        if (key == overlay_keys[item]) {
            const unsigned bit = 1u << item;
            v.overlay_items = i ? (v.overlay_items | bit) : (v.overlay_items & ~bit);
            return;
        }
    }
    if (key == "overlay_scale") {
        v.overlay_scale = std::clamp(i, 50, 300);
        return;
    }
    if (key == "overlay_corner") {
        v.overlay_corner = i >= 0 && i <= 3 ? i : 1;
        return;
    }
    if (key == "overlay_opacity") {
        v.overlay_opacity = std::clamp(i, 0, 100);
        return;
    }
    if (key == "overlay_layout") {
        v.overlay_layout = i == 1 ? 1 : 0;
        return;
    }
    if (key == "overlay_advanced") {
        v.overlay_advanced = i != 0;
        return;
    }
    if (key == "menu_tab") {
        v.menu_tab = i >= 0 && i < MenuTabCount ? i : 0;
        return;
    }
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "display_mode") {
        v.display_mode = PortGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = i != 0;
    } else if (key == "fps_limit") {
        v.fps_limit = PortGraphics::ParseFpsLimit(i);
    } else if (key == "async_shaders") {
        v.async_shaders = i != 0;
    } else if (key == "pause_game") {
        v.pause_game = i != 0;
    } else if (key == "ui_language") {
        // "en" is English; any other unknown code (or "auto") follows Windows.
        const int found = PortUi::FindLanguage(value);
        v.ui_language = found > 0 || value.starts_with("en") ? found : -1;
    } else if (key == "fsr1") {
        // FSR 1 left the launcher and the in-game menu: a value saved by an older version has no
        // control left to turn it off, so it stays off.
        v.fsr1 = false;
    } else if (key == "rcas") {
        v.rcas = i != 0;
    } else if (key == "rcas_strength") {
        v.rcas_strength = PortGraphics::ParseRcasStrength(value.empty() ? -1 : i);
    } else if (key == "frames_queued") {
        v.frames_queued = PortGraphics::ParseFramesQueued(value.empty() ? -1 : i);
    } else if (key == "mouse_look") {
        v.mouse_look = i != 0;
    } else if (key == "mouse_sensitivity") {
        v.mouse_sensitivity = std::clamp(i, 10, 400);
    } else if (key == "mouse_sensitivity_y") {
        v.mouse_sensitivity_y = std::clamp(i, 10, 400);
    } else if (key == "mouse_invert_y") {
        v.mouse_invert_y = i != 0;
    } else if (key == "fov") {
        v.fov = std::clamp(i, 50, 200);
    } else if (key == "anisotropy") {
        v.anisotropy = i >= 16 ? 16 : i >= 8 ? 8 : i >= 4 ? 4 : i >= 2 ? 2 : 0;
    } else if (key == "aspect_ratio") {
        v.aspect = PortGraphics::ParseAspect(value);
    } else if (key == "engine_fps") {
        v.engine_fps = PortGraphics::ParseEngineFps(i);
    } else if (key == "deferred_readback") {
        v.deferred_readback = i != 0;
    } else if (key == "stale_readback") {
        v.stale_readback = i != 0;
    } else if (key == "restart_unconfirmed") {
        v.restart_unconfirmed = i != 0;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "master_volume") {
        // Not a number (or empty): the game's own level, never a silent game.
        const bool numeric = !value.empty() && (std::isdigit(static_cast<unsigned char>(value[0])) ||
                                                ((value[0] == '-' || value[0] == '+') && value.size() > 1 &&
                                                 std::isdigit(static_cast<unsigned char>(value[1]))));
        v.master_volume = numeric ? std::clamp(i, MasterVolumeMin, MasterVolumeMax) : MasterVolumeDefault;
    } else if (key == "audio_channels") {
        v.audio_channels = ParseAudioChannels(value);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("THEORDER_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"THEORDER_FSR_SHARPNESS", "sharpness"},        {"THEORDER_JITTER", "jitter"},
        {"THEORDER_REACTIVE", "reactive"},              {"THEORDER_REACTIVE_SCALE", "reactive_scale"},
        {"THEORDER_REACTIVE_THRESHOLD", "reactive_threshold"}, {"THEORDER_REACTIVE_MAX", "reactive_max"},
        {"THEORDER_UPSCALE_PRESET", "preset"},            {"THEORDER_OBJECT_MOTION", "object_motion"},
        {"THEORDER_ASYNC_SHADERS", "async_shaders"},      {"THEORDER_DEFERRED_READBACK", "deferred_readback"},
        {"THEORDER_STALE_READBACK", "stale_readback"},     {"THEORDER_FSR1", "fsr1"},
        {"THEORDER_RCAS", "rcas"},                        {"THEORDER_FRAMES_AHEAD", "frames_queued"},
        {"THEORDER_ANISOTROPY", "anisotropy"},
        {"THEORDER_UI_LANGUAGE", "ui_language"},
        {"THEORDER_AUDIO_VOLUME", "master_volume"},       {"THEORDER_AUDIO_CHANNELS", "audio_channels"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_aspect = v.aspect;
    v.startup_engine_fps = v.engine_fps;
    const char* applied = std::getenv("THEORDER_ENGINE_FPS");
    v.applied_engine_fps = applied && applied[0] ? PortGraphics::ParseEngineFps(std::atoi(applied))
                                                 : v.engine_fps.load();
    v.startup_deferred_readback = v.deferred_readback;
    v.startup_stale_readback = v.stale_readback;
    v.startup_anisotropy = v.anisotropy;
}

bool GraphicsNeedRestart() {
    const auto& v = Get();
    return v.aspect != v.startup_aspect ||
           v.engine_fps != v.startup_engine_fps ||
           v.deferred_readback != v.startup_deferred_readback ||
           v.stale_readback != v.startup_stale_readback;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "port_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("THEORDER_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# port settings (in-game menu: Insert / R3+L2)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    std::fprintf(file, "output_res=%dx%d\n", OutputWidths[v.output_res], OutputHeights[v.output_res]);
    std::fprintf(file,
                 "overlay_fps=%d\noverlay_frametime=%d\noverlay_upscaler=%d\noverlay_gpu=%d\n"
                 "overlay_cpu=%d\noverlay_ram=%d\noverlay_vram=%d\noverlay_scale=%d\noverlay_corner=%d\n"
                 "overlay_opacity=%d\noverlay_layout=%d\nmenu_tab=%d\noverlay_advanced=%d\n",
                 int(v.overlay_items >> 0 & 1), int(v.overlay_items >> 1 & 1), int(v.overlay_items >> 2 & 1),
                 int(v.overlay_items >> 3 & 1), int(v.overlay_items >> 4 & 1), int(v.overlay_items >> 5 & 1),
                 int(v.overlay_items >> 6 & 1), v.overlay_scale.load(), v.overlay_corner.load(),
                 v.overlay_opacity.load(), v.overlay_layout.load(), v.menu_tab.load(),
                 int(v.overlay_advanced.load()));
    std::fprintf(file,
                 "display_mode=%s\nvsync=%d\nfps_limit=%d\nasync_shaders=%d\n"
                 "fsr1=%d\nrcas=%d\nrcas_strength=%d\nframes_queued=%d\n"
                 "aspect_ratio=%s\nengine_fps=%d\ndeferred_readback=%d\nstale_readback=%d\n"
                 "restart_unconfirmed=%d\n",
                 PortGraphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)].data(),
                 int(v.vsync.load()), v.fps_limit.load(), int(v.async_shaders.load()),
                 int(v.fsr1.load()), int(v.rcas.load()), v.rcas_strength.load(), v.frames_queued.load(),
                 PortGraphics::Aspects[std::clamp(v.aspect.load(), 0, int(PortGraphics::Aspects.size()) - 1)].data(),
                 v.engine_fps.load(), int(v.deferred_readback.load()), int(v.stale_readback.load()),
                 int(v.restart_unconfirmed.load()));
    std::fprintf(file, "fov=%d\nmouse_look=%d\nmouse_sensitivity=%d\nmouse_sensitivity_y=%d\nmouse_invert_y=%d\nanisotropy=%d\n",
                 v.fov.load(), int(v.mouse_look.load()), v.mouse_sensitivity.load(),
                 v.mouse_sensitivity_y.load(), int(v.mouse_invert_y.load()), v.anisotropy.load());
    {
        const int language = v.ui_language.load();
        std::fprintf(file, "ui_language=%s\n",
                     language >= 0 && language < PortUi::LanguageCount
                         ? std::string(PortUi::Languages[language].code).c_str()
                         : "auto");
    }
    std::fprintf(file, "pause_game=%d\n", int(v.pause_game.load()));
    std::fprintf(file, "master_volume=%d\naudio_channels=%s\n", v.master_volume.load(),
                 AudioChannelKeys[std::clamp(v.audio_channels.load(), 0, AudioChannelsCount - 1)].data());
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    std::fclose(file);
}

namespace {
std::vector<std::pair<double*, double>> fov_sites;
}

void SetFovSites(double* const* sites, int count) {
    for (int i = 0; i < count; ++i) {
        fov_sites.emplace_back(sites[i], *sites[i]);
    }
    ApplyFov();
}

void ApplyFov() {
    const double scale = Get().fov.load() / 100.0;
    for (const auto& [site, base] : fov_sites) {
        *site = base * scale; // the game reads it every frame; an aligned double store is atomic
    }
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace PortSettings
