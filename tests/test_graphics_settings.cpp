// theorder.ini graphics keys (port_settings.cpp, port_graphics.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "port_graphics.h"
#include "port_settings.h"

namespace G = PortGraphics;

static void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    const int result = _putenv_s(name, value);
#else
    const int result = value[0] ? setenv(name, value, 1) : unsetenv(name);
#endif
    assert(result == 0);
}

static std::string Read(const char* path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

int main() {
    assert(G::ParseDisplayMode("borderless") == G::Borderless && G::ParseDisplayMode("bogus") == G::Windowed);
    assert(G::ParseFpsLimit(240) == 240 && G::ParseFpsLimit(75) == 0);
    assert(G::ParseAspect("16:10") == 2 && G::ParseAspect("21:9") == 3);
    assert(G::ParseAspect("32:9") == 4 && G::ParseAspect("4:3") == 0);
    assert(G::Aspects.size() == 5 && G::AspectNames.size() == 5 && G::AspectValues.size() == 5);
    assert(G::Aspects[4] == "32:9" && G::AspectValues[4] == 32.0f / 9.0f);
    // The picture an aspect ratio patch leaves in the 1920x1080 display buffer (measured in dumps).
    const auto wide = G::PictureRect(1920, 1080, G::AspectValues[3]);
    assert(wide.x == 0 && wide.y == 135 && wide.w == 1920 && wide.h == 810);
    const auto tall = G::PictureRect(1920, 1080, G::AspectValues[2]);
    assert(tall.x == 96 && tall.y == 0 && tall.w == 1728 && tall.h == 1080);
    const auto original = G::PictureRect(1920, 1080, G::AspectValues[0]);
    assert(original.y == 140 && original.h == 800);
    const auto full = G::PictureRect(1920, 1080, G::AspectValues[1]);
    assert(full.x == 0 && full.y == 0 && full.w == 1920 && full.h == 1080);
    const auto ultrawide = G::PictureRect(1920, 1080, G::AspectValues[4]);
    assert(ultrawide.x == 0 && ultrawide.y == 270 && ultrawide.w == 1920 && ultrawide.h == 540);
    const auto full_ultrawide = G::PictureRect(5120, 1440, G::AspectValues[4]);
    assert(full_ultrawide.x == 0 && full_ultrawide.y == 0 &&
           full_ultrawide.w == 5120 && full_ultrawide.h == 1440);
    assert(PortSettings::OutputCount == 8 && PortSettings::OutputDefault == 1);
    assert(PortSettings::OutputWidths[0] == 1280 && PortSettings::OutputHeights[0] == 720);
    assert(PortSettings::OutputWidths[1] == 1920 && PortSettings::OutputHeights[1] == 1080);
    assert(PortSettings::OutputWidths[2] == 2560 && PortSettings::OutputHeights[2] == 1440);
    assert(PortSettings::OutputWidths[3] == 3840 && PortSettings::OutputHeights[3] == 2160);
    assert(PortSettings::OutputWidths[4] == 5120 && PortSettings::OutputHeights[4] == 1440);
    assert(PortSettings::OutputWidths[6] == 3440 && PortSettings::OutputHeights[6] == 1440);
    assert(PortSettings::OutputWidths[7] == 3840 && PortSettings::OutputHeights[7] == 1080);
    assert(G::ParseEngineFps(240) == 240 && G::ParseEngineFps(90) == 120);
    assert(G::ParseFramesQueued(2) == 2 && G::ParseFramesQueued(0) == 0 && G::ParseFramesQueued(5) == 1);
    assert(G::ParseRcasStrength(100) == 100 && G::ParseRcasStrength(101) == 75 && G::ParseRcasStrength(-1) == 75);
    assert(G::RcasAttenuation(100) == 0.0f && G::RcasAttenuation(0) == 2.0f && G::RcasAttenuation(75) == 0.5f);
    assert(G::FsrPassRuns(true, false, 1280, 720, 2560, 1440) && !G::FsrPassRuns(false, false, 1280, 720, 2560, 1440));
    assert(G::FsrPassRuns(false, true, 1920, 1080, 1920, 1080) && !G::FsrPassRuns(true, false, 1920, 1080, 1920, 1080));
    assert(!G::FsrPassRuns(true, true, 2560, 1440, 1920, 1080));  // larger image: regular scaling
    assert(G::EffectiveFps(0, 120) == 120 && G::EffectiveFps(240, 120) == 120 && G::EffectiveFps(60, 240) == 60);
    // An unpatched engine steps 1/60 s per frame: presenting faster speeds the game up.
    assert(G::PresentLimit(120, 60) == 60 && G::PresentLimit(240, 120) == 120 && G::PresentLimit(240, 240) == 240);
    assert(G::PresentLimit(0, 60) == 0 && G::PresentLimit(30, 60) == 30);  // 0: paced by vblank

    const char* path = "graphics-settings-test.ini";
    {
        std::ofstream out(path);
        out << "display_mode=borderless\nvsync=0\nfps_limit=240\nasync_shaders=0\n"
               "aspect_ratio=16:10\nengine_fps=240\ndeferred_readback=0\nstale_readback=1\n"
               "fsr1=1\nrcas=0\nrcas_strength=40\nframes_queued=2\n";
    }
    SetEnv("THEORDER_CONFIG", path);
    SetEnv("THEORDER_ASYNC_SHADERS", "");
    auto& v = PortSettings::Get();
    PortSettings::Load();
    assert(v.output_res == PortSettings::OutputDefault);
    assert(v.display_mode == G::Borderless && !v.vsync && v.fps_limit == 240 && !v.async_shaders);
    assert(v.aspect == 2 && v.engine_fps == 240 && !v.deferred_readback && v.stale_readback);
    // FSR 1 left the menus in 2.2.0: a saved fsr1=1 (or THEORDER_FSR1) no longer turns it on.
    assert(!v.fsr1 && !v.rcas && v.rcas_strength == 40 && v.frames_queued == 2);
    SetEnv("THEORDER_FSR1", "1");
    PortSettings::Load();
    assert(!v.fsr1);
    SetEnv("THEORDER_FSR1", "");
    assert(!PortSettings::GraphicsNeedRestart());
    assert(v.applied_engine_fps == 240);  // no THEORDER_ENGINE_FPS: the configured rate
    SetEnv("THEORDER_ENGINE_FPS", "60");    // run.py: the frame rate patch did not apply
    PortSettings::Load();
    assert(v.engine_fps == 240 && v.applied_engine_fps == 60 && !PortSettings::GraphicsNeedRestart());
    SetEnv("THEORDER_ENGINE_FPS", "");
    v.engine_fps = 120;
    assert(PortSettings::GraphicsNeedRestart());
    v.fps_limit = 60;  // live: no restart because of it
    v.engine_fps = 240;
    assert(!PortSettings::GraphicsNeedRestart());

    PortSettings::Save();
    const std::string saved = Read(path);
    for (const char* line : {"display_mode=borderless\n", "vsync=0\n", "fps_limit=60\n", "async_shaders=0\n",
                             "aspect_ratio=16:10\n", "engine_fps=240\n", "deferred_readback=0\n",
                             "stale_readback=1\n", "fsr1=0\n", "rcas=0\n", "rcas_strength=40\n",
                             "frames_queued=2\n", "output_res=1920x1080\n"}) {
        assert(saved.find(line) != std::string::npos);
    }

    {
        std::ofstream out(path);
        out << "aspect_ratio=32:9\noutput_res=5120x1440\n";
    }
    PortSettings::Load();
    assert(v.aspect == 4 && v.startup_aspect == 4);
    assert(v.output_res == 4 && v.startup_output_res == 4);
    assert(!PortSettings::GraphicsNeedRestart());
    v.aspect = 1;
    assert(PortSettings::GraphicsNeedRestart());
    v.aspect = 4;
    assert(!PortSettings::GraphicsNeedRestart());
    PortSettings::Save();
    const std::string saved_ultrawide = Read(path);
    assert(saved_ultrawide.find("aspect_ratio=32:9\n") != std::string::npos);
    assert(saved_ultrawide.find("output_res=5120x1440\n") != std::string::npos);
    v.aspect = 0;
    v.output_res = PortSettings::OutputDefault;
    PortSettings::Load();
    assert(v.aspect == 4 && v.startup_aspect == 4 && v.output_res == 4 && v.startup_output_res == 4);

    {
        std::ofstream out(path);
        out << "output_res=3440x1440\n";
    }
    PortSettings::Load();
    assert(v.output_res == 6 && PortSettings::OutputWidths[v.output_res] == 3440 &&
           PortSettings::OutputHeights[v.output_res] == 1440);
    PortSettings::Save();
    assert(Read(path).find("output_res=3440x1440\n") != std::string::npos);

    {
        std::ofstream out(path);
        out << "output_res=3840x1080\n";
    }
    PortSettings::Load();
    assert(v.output_res == 7 && PortSettings::OutputWidths[v.output_res] == 3840 &&
           PortSettings::OutputHeights[v.output_res] == 1080);
    PortSettings::Save();
    assert(Read(path).find("output_res=3840x1080\n") != std::string::npos);

    {
        std::ofstream out(path);
        out << "output_res=1600x900\n";
    }
    PortSettings::Load();
    assert(PortSettings::OutputWidths[v.output_res] == 1600 &&
           PortSettings::OutputHeights[v.output_res] == 900);
    PortSettings::Save();
    assert(Read(path).find("output_res=1600x900\n") != std::string::npos);

    {
        // Audio (src/runtime_audio.c, SpartanPort 1e95c7c): the master volume is 0 to 200 percent,
        // the speaker layout is named by key, anything unknown is automatic; both survive a save
        // and the environment wins.
        namespace S = PortSettings;
        assert(S::ParseAudioChannels("auto") == S::AudioChannelsAuto && S::ParseAudioChannels("stereo") == S::AudioStereo);
        assert(S::ParseAudioChannels("5.1") == S::Audio51 && S::ParseAudioChannels("7.1") == S::Audio71);
        assert(S::ParseAudioChannels("2.0") == S::AudioStereo && S::ParseAudioChannels("6") == S::Audio51);
        assert(S::ParseAudioChannels("8") == S::Audio71 && S::ParseAudioChannels("") == S::AudioChannelsAuto);
        assert(S::ParseAudioChannels("bogus") == S::AudioChannelsAuto && S::ParseAudioChannels("7") == S::AudioChannelsAuto);
        for (int c = 0; c < S::AudioChannelsCount; ++c) assert(S::ParseAudioChannels(S::AudioChannelKeys[c]) == c);
        {
            std::ofstream out(path);
            out << "master_volume=150\naudio_channels=5.1\n";
        }
        S::Load();
        assert(v.master_volume == 150 && v.audio_channels == S::Audio51);
        S::Save();
        const std::string audio_ini = Read(path);
        assert(audio_ini.find("master_volume=150\n") != std::string::npos);
        assert(audio_ini.find("audio_channels=5.1\n") != std::string::npos);
        {
            std::ofstream out(path);
            out << "master_volume=900\naudio_channels=7.1\n";
        }
        S::Load();
        assert(v.master_volume == 200 && v.audio_channels == S::Audio71);
        {
            std::ofstream out(path);
            out << "master_volume=-5\naudio_channels=bogus\n";
        }
        S::Load();
        assert(v.master_volume == 0 && v.audio_channels == S::AudioChannelsAuto);
        {
            std::ofstream out(path);
            out << "master_volume=\n";
        }
        S::Load();
        assert(v.master_volume == 100);
        {
            std::ofstream out(path);
            out << "master_volume=abc\n"; // not a number: the game's own level, not silence
        }
        S::Load();
        assert(v.master_volume == 100);
        SetEnv("THEORDER_AUDIO_VOLUME", "80");
        SetEnv("THEORDER_AUDIO_CHANNELS", "stereo");
        S::Load();
        assert(v.master_volume == 80 && v.audio_channels == S::AudioStereo);
        SetEnv("THEORDER_AUDIO_VOLUME", "");
        SetEnv("THEORDER_AUDIO_CHANNELS", "");
    }
    {
        // Frame generation (SpartanPort 8192bbb): off / fsr (also 0 / 1), anything else off; the
        // startup value is what the swapchain and the presenter use; the environment wins.
        namespace S = PortSettings;
        for (const auto& [text, want] : {std::pair{"fsr", 1}, std::pair{"1", 1}, std::pair{"off", 0},
                                         std::pair{"0", 0}, std::pair{"FSR", 0}, std::pair{"", 0}}) {
            {
                std::ofstream out(path);
                out << "frame_generation=" << text << "\n";
            }
            S::Load();
            assert(v.frame_generation == want && v.startup_frame_generation == want);
        }
        v.frame_generation = 1;
        S::Save();
        assert(Read(path).find("frame_generation=fsr\n") != std::string::npos);
        v.frame_generation = 0;
        S::Save();
        assert(Read(path).find("frame_generation=off\n") != std::string::npos);
        SetEnv("THEORDER_FRAME_GENERATION", "fsr");
        S::Load();
        assert(v.frame_generation == 1 && v.startup_frame_generation == 1);
        SetEnv("THEORDER_FRAME_GENERATION", "");
        v.frame_generation = 0;
    }

    {
        std::ofstream out(path);
        out << "display_mode=huge\nfps_limit=7\naspect_ratio=4:3\nengine_fps=1\n"
               "rcas_strength=250\nframes_queued=9\n";
    }
    SetEnv("THEORDER_ASYNC_SHADERS", "1");  // the environment still wins at start
    v.async_shaders = false;
    PortSettings::Load();
    assert(v.display_mode == G::Windowed && v.fps_limit == 0 && v.aspect == 0 && v.engine_fps == 120);
    assert(v.rcas_strength == 75 && v.frames_queued == 1);
    assert(v.async_shaders);
    std::remove(path);
    std::puts("PASS: graphics settings");
}
