// Exercise the real event handler without a window, Vulkan backend or guest.
#include "../gpu/theordergpu.h"
#include "../gpu/shim/port_overlay.cpp"

// The menu's Exit to Desktop (theordergpu.cpp) is not reached here.
extern "C" void theordergpu_exit_to_desktop(void) { std::abort(); }

namespace Core {
class Emulator {
public:
    static void FillElfInfo(const TheOrderGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};
}

static void CheckAt(bool condition, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: overlay input isolation (line %d)\n", line);
    std::exit(1);
}
#define Check(condition) CheckAt((condition), __LINE__)

static void Key(SDL_Keycode key, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.key = key;
    PortOverlay::HandleEvent(event);
}

static void Mouse(bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_LEFT;
    PortOverlay::HandleEvent(event);
}

static void Gamepad(bool down, SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.button = button;
    PortOverlay::HandleEvent(event);
}

static void Trigger(bool down) {
    SDL_Event event{};
    event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    event.gaxis.axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    event.gaxis.value = down ? 20000 : 0;
    PortOverlay::HandleEvent(event);
}

static ImGuiWindow* MenuFrame() {
    ImGui::NewFrame();
    PortOverlay::Menu();
    ImGui::EndFrame();
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && !(window->Flags & ImGuiWindowFlags_ChildWindow) &&
            std::strstr(window->Name, "(Insert")) {
            return window;
        }
    }
    Check(false);
    return nullptr;
}

int main() {
    Check(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc descriptor{};
    SDL_INIT_INTERFACE(&descriptor);
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    descriptor.name = "Overlay input regression";
    const SDL_JoystickID device = SDL_AttachVirtualJoystick(&descriptor);
    Check(device != 0);
    SDL_Gamepad* controller = SDL_OpenGamepad(device);
    Check(controller != nullptr);
    ImGui::CreateContext();
    PortOverlay::initialized = true;
    auto& io = ImGui::GetIO();
    Key(SDLK_INSERT, true);
    Check(PortOverlay::CapturesInput());
    Key(SDLK_DOWN, true);
    Mouse(true);
    Gamepad(true);
    ImGui::UpdateInputEvents(false);
    Check(ImGui::IsKeyDown(ImGuiKey_DownArrow));
    Check(io.MouseDown[0]);
    Check(ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown));

    Key(SDLK_INSERT, true);
    Key(SDLK_DOWN, false);
    Mouse(false);
    Gamepad(false);
    Key(SDLK_INSERT, true);
    ImGui::UpdateInputEvents(false);
    std::printf("reopen: keyboard=%d mouse=%d gamepad=%d\n",
                ImGui::IsKeyDown(ImGuiKey_DownArrow), io.MouseDown[0],
                ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown));
    std::fflush(stdout);
    Check(!ImGui::IsKeyDown(ImGuiKey_DownArrow));
    Check(!io.MouseDown[0]);
    Check(!ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown));

    // Also close before queued presses have reached a rendered ImGui frame.
    Key(SDLK_DOWN, true);
    Mouse(true);
    Key(SDLK_INSERT, true);
    Key(SDLK_DOWN, false);
    Mouse(false);
    Key(SDLK_INSERT, true);
    ImGui::UpdateInputEvents(false);
    Check(!ImGui::IsKeyDown(ImGuiKey_DownArrow));
    Check(!io.MouseDown[0]);
    Key(SDLK_DOWN, true);
    Mouse(true);
    Gamepad(true);
    ImGui::UpdateInputEvents(false);
    Check(ImGui::IsKeyDown(ImGuiKey_DownArrow) && io.MouseDown[0]);
    Check(ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown));
    Key(SDLK_DOWN, false);
    Mouse(false);
    Gamepad(false);
    ImGui::UpdateInputEvents(false);
    Check(!ImGui::IsKeyDown(ImGuiKey_DownArrow) && !io.MouseDown[0]);
    Check(!ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown));

    Key(SDLK_DOWN, true);
    Mouse(true);
    Gamepad(true);
    ImGui::UpdateInputEvents(false);
    Trigger(true);
    Gamepad(true, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    Check(!PortOverlay::CapturesInput());
    Key(SDLK_DOWN, false);
    Mouse(false);
    Gamepad(false);
    Gamepad(false, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    Trigger(false);
    // Reverse chord order also opens through the same transition.
    Gamepad(true, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    Trigger(true);
    Check(PortOverlay::CapturesInput());
    ImGui::UpdateInputEvents(false);
    Check(!ImGui::IsKeyDown(ImGuiKey_DownArrow) && !io.MouseDown[0]);
    Check(!ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown));
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 100.0f;
    motion.motion.y = 100.0f;
    motion.motion.xrel = 1.0f;
    PortOverlay::HandleEvent(motion);
    Check(PortOverlay::last_active_input == PortOverlay::ActiveInput::Keyboard);
    SDL_Event noise{};
    noise.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    noise.gaxis.which = device;
    noise.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    noise.gaxis.value = 512;
    PortOverlay::HandleEvent(noise);
    std::printf("idle controller preserves mouse: %d\n",
                PortOverlay::last_active_input == PortOverlay::ActiveInput::Keyboard);
    Check(PortOverlay::last_active_input == PortOverlay::ActiveInput::Keyboard);
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 480);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault();
    Check(io.Fonts->Build());
    io.ClearInputKeys();
    io.ClearInputMouse();
    io.ClearEventsQueue();
    TheOrderGpuConfig config{};
    config.title = "The Order: 1886";
    Core::Emulator::FillElfInfo(config);
    PortOverlay::requested_tab = 3;
    PortOverlay::last_active_input = PortOverlay::ActiveInput::Keyboard;
    ImGuiWindow* keyboard_window = MenuFrame();
    MenuFrame();
    PortOverlay::last_active_input = PortOverlay::ActiveInput::Xbox;
    ImGuiWindow* controller_window = MenuFrame();
    std::printf("menu identity survives device switch: %d\n", keyboard_window == controller_window);
    Check(keyboard_window == controller_window);
    Check(controller_window->DC.ChildWindows.Size == 1);
    ImGuiWindow* body = controller_window->DC.ChildWindows[0];
    Check(body->ScrollMax.y > 0.0f);
    motion.motion.x = body->Pos.x + body->Size.x * 0.5f;
    motion.motion.y = body->Pos.y + body->Size.y * 0.5f;
    PortOverlay::HandleEvent(motion);
    MenuFrame();
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = -3.0f;
    wheel.wheel.mouse_x = motion.motion.x;
    wheel.wheel.mouse_y = motion.motion.y;
    PortOverlay::HandleEvent(wheel);
    for (int frame = 0; frame < 4; ++frame) MenuFrame();
    std::printf("mouse wheel scroll: %.1f of %.1f\n", body->Scroll.y, body->ScrollMax.y);
    Check(body->Scroll.y > 0.0f);
    const float scroll_before_switch = body->Scroll.y;
    PortOverlay::last_active_input = PortOverlay::ActiveInput::Xbox;
    MenuFrame();
    Check(std::abs(body->Scroll.y - scroll_before_switch) < 0.1f);
    ImGui::SetScrollY(body, 0.0f);
    MenuFrame();
    wheel.wheel.y = 3.0f;
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    PortOverlay::HandleEvent(wheel);
    for (int frame = 0; frame < 4; ++frame) MenuFrame();
    std::printf("inverted wheel scroll: %.1f\n", body->Scroll.y);
    Check(body->Scroll.y > 0.0f);
    ImGui::SetScrollY(body, 0.0f);
    MenuFrame();
    SDL_Event stick{};
    stick.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    stick.gaxis.which = device;
    stick.gaxis.axis = SDL_GAMEPAD_AXIS_RIGHTY;
    stick.gaxis.value = 24000;
    Check(PortOverlay::HandleEvent(stick));
    ImGui::UpdateInputEvents(false);
    Check(ImGui::IsKeyDown(ImGuiKey_GamepadRStickDown));
    for (int frame = 0; frame < 30; ++frame) MenuFrame();
    std::printf("right stick scroll: %.1f\n", body->Scroll.y);
    Check(body->Scroll.y > 0.0f);
    PortOverlay::HandleEvent(motion);
    ImGui::UpdateInputEvents(false);
    Check(!ImGui::IsKeyDown(ImGuiKey_GamepadRStickDown));
    MenuFrame();
    const float scroll_after_mouse = body->Scroll.y;
    for (int frame = 0; frame < 8; ++frame) MenuFrame();
    Check(std::abs(body->Scroll.y - scroll_after_mouse) < 0.1f);
    stick.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTY;
    Check(PortOverlay::HandleEvent(stick));
    ImGui::UpdateInputEvents(false);
    Check(ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown));
    stick.gaxis.value = 0;
    Check(PortOverlay::HandleEvent(stick));
    ImGui::UpdateInputEvents(false);
    Check(!ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown));
    PortOverlay::requested_tab = 3;
    MenuFrame();
    SDL_Event previous_tab{};
    previous_tab.type = SDL_EVENT_KEY_DOWN;
    previous_tab.key.key = SDLK_TAB;
    previous_tab.key.mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT;
    PortOverlay::HandleEvent(previous_tab);
    MenuFrame();
    std::printf("reverse tab: %d\n", PortSettings::Get().menu_tab.load());
    Check(PortSettings::Get().menu_tab == 2);
    previous_tab.type = SDL_EVENT_KEY_UP;
    previous_tab.key.mod = SDL_KMOD_NONE;
    PortOverlay::HandleEvent(previous_tab);
    MenuFrame();
    // The Game tab (index 4) renders; the open menu pauses the game unless pause_game is off or
    // a loading screen is up (SpartanPort 6853390).
    PortOverlay::requested_tab = 4;
    MenuFrame();
    Check(PortSettings::Get().menu_tab == 4);
    Check(PortOverlay::GamePaused());
    PortSettings::Get().pause_game = false;
    Check(!PortOverlay::GamePaused());
    PortOverlay::WaitWhilePaused(); // returns at once
    PortSettings::Get().pause_game = true;
    PortOverlay::loading = true;
    Check(!PortOverlay::GamePaused());
    PortOverlay::loading = false;
    Check(PortOverlay::GamePaused());
    {
        // A game thread really stops in WaitWhilePaused while the menu is open, and goes on once
        // it closes (the flip and sound entry points call it).
        std::atomic<bool> resumed{false};
        std::thread game([&] {
            PortOverlay::WaitWhilePaused("frame");
            resumed = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Check(!resumed);
        PortOverlay::SetOpen(false);
        for (int i = 0; i < 200 && !resumed; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(resumed);
        game.join();
        PortOverlay::SetOpen(true);
        PortOverlay::requested_tab = 4;
        MenuFrame();
    }
    {
        // Exit to Desktop is reachable with the arrow keys / d-pad. Enter asks the question; Escape
        // cancels it and leaves the menu open.
        const auto press = [](SDL_Keycode key) {
            Key(key, true);
            MenuFrame();
            Key(key, false);
            MenuFrame();
        };
        PortOverlay::SetOpen(false); // opened afresh, as with Insert in the game
        PortOverlay::SetOpen(true);
        PortOverlay::requested_tab = 4;
        MenuFrame();
        MenuFrame();
        // From Close (the first focus): up to the tab bar, then down through Pause the game,
        // Master volume, Speakers and Menu language to Exit to Desktop. The body is NavFlattened:
        // without it the arrows went from the tab bar straight to Close.
        press(SDLK_UP);
        for (int i = 0; i < 5; ++i) {
            press(SDLK_DOWN);
        }
        Check(ImGui::GetTopMostPopupModal() == nullptr);
        press(SDLK_RETURN);
        ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        Check(modal && std::strstr(modal->Name, "Exit to Desktop?"));
        press(SDLK_ESCAPE);
        Check(ImGui::GetTopMostPopupModal() == nullptr);
        Check(PortOverlay::menu_open);
    }
    PortOverlay::dirty = false;
    PortOverlay::SetOpen(false);
    Check(!PortOverlay::GamePaused());
    Gamepad(false, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    Trigger(true);
    SDL_Event focus_lost{};
    focus_lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    PortOverlay::HandleEvent(focus_lost);
    Check(!PortOverlay::r3_down && !PortOverlay::l2_down);
    Gamepad(true, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    Check(!PortOverlay::CapturesInput());
    PortOverlay::initialized = false;
    ImGui::DestroyContext();
    SDL_CloseGamepad(controller);
    Check(SDL_DetachVirtualJoystick(device));
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    std::puts("PASS: overlay close/reopen input isolation");
}
