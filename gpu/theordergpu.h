/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef THEORDERGPU_H
#define THEORDERGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} TheOrderGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void theordergpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int theordergpu_init(const TheOrderGpuConfig *config);
/* Unless THEORDER_CACHE_FLUSH_ON_EXIT=0: waits at most two seconds for queued shader-cache
 * writes before a window close or restart skips destructors (SpartanPort c22329f). */
void theordergpu_flush_cache(void);
/* Master volume of the game's audio, 0 to 200 percent (theorder.ini master_volume,
 * THEORDER_AUDIO_VOLUME; 100: the game's own mix, more is boosted behind a soft limiter). */
int theordergpu_audio_volume_percent(void);
/* Speaker layout for the game's 7.1 audio (theorder.ini audio_channels, THEORDER_AUDIO_CHANNELS):
 * 0 automatic (the device's), 1 stereo, 2 5.1, 3 7.1. Read when an audio port is opened. */
int theordergpu_audio_channels(void);
/* Game threads (audio output): returns once the in-game menu no longer pauses the game. */
void theordergpu_wait_while_paused(void);
/* The menu's Exit to Desktop: the same as closing the window (report, shader-cache flush, exit). */
void theordergpu_exit_to_desktop(void);
/* runtime_services.c: a trophy was just unlocked (name, description, icon PNG or null). */
void theordergpu_trophy_unlocked(int id, const char *name, const char *detail,
                                 const unsigned char *png, uint64_t png_size);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t theordergpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int theordergpu_handle_fault(void *ucontext, void *address);
/* THEORDER_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void theordergpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int theordergpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int theordergpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int theordergpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int theordergpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int theordergpu_overlay_captures_input(void);
/* The game's field of view constants in the loaded image (writable): the in-game menu scales them. */
void theordergpu_set_fov_sites(double *const *sites, int count);
/* Mouse input since the last call (window thread, gpu/shim/window.cpp): motion in pixels,
 * buttons as SDL_BUTTON_MASK bits; all zero unless captured (mouse look on, window focused,
 * menu and text entry closed). sensitivity: the menu's, 1.0 = 100%. */
typedef struct {
    float dx, dy, sensitivity;
    uint32_t buttons;
    int captured;
    float sens_y;
    int invert_y;
} TheOrderMouse;
void theordergpu_mouse_take(TheOrderMouse *out);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned theordergpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
