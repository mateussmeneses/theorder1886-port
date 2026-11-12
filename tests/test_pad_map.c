/* THEORDER_PAD_MAP / THEORDER_KEY_MAP parsing (src/runtime_pad.c). */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"
#ifdef _WIN32
#include <windows.h>
#endif

int theordergpu_overlay_captures_input(void) { return 0; }
/* This map-only executable links runtime_pad.c without the GPU shim. */
static TheOrderMouse mouse_sample;
void theordergpu_mouse_take(TheOrderMouse *out) { *out=mouse_sample; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* PS4 buttons a host source presses in `map`. */
static uint32_t pressed_by(const ButtonMap *map, int source) {
    uint32_t ps=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].source==source) ps|=map->entries[i].ps;
    return ps;
}

static void test_mouse_sample_rate(void) {
    const unsigned rates[]={30,60,120,1000};
    int reference=0;
    for (size_t rate_index=0;rate_index<sizeof(rates)/sizeof(*rates);++rate_index) {
        mouse_sample=(TheOrderMouse){0};
        PadData sample={.right_x=128,.right_y=128,.timestamp=1000000};
        mouse_input(&sample);
        mouse_sample=(TheOrderMouse){.captured=1,.sensitivity=1.0f,.sens_y=1.0f};
        mouse_input(&sample);
        uint64_t previous=sample.timestamp;
        for (unsigned step=1;step<=rates[rate_index];++step) {
            sample=(PadData){.right_x=128,.right_y=128,
                .timestamp=1000000+(uint64_t)step*1000000/rates[rate_index]};
            mouse_sample.dx=300.0f*(float)(sample.timestamp-previous)/1000000.0f;
            mouse_input(&sample);
            previous=sample.timestamp;
        }
        const int deflection=(int)sample.right_x-128;
        printf("mouse sampling: %u Hz -> %d\n",rates[rate_index],deflection);
        fflush(stdout);
        if (!rate_index) reference=deflection;
        if (abs(deflection-reference)>1) {
            fprintf(stderr,"FAIL: mouse response changes with sampling rate\n");
            exit(1);
        }
        mouse_sample.dx=0.0f;
        for (unsigned stopped=1;stopped<=2;++stopped) {
            sample=(PadData){.right_x=128,.right_y=128,.timestamp=previous+stopped*20000};
            mouse_input(&sample);
        }
        assert(sample.right_x==128 && sample.right_y==128);
    }
    mouse_sample=(TheOrderMouse){0};
    PadData sample={.right_x=128,.right_y=128,.timestamp=3000000};
    mouse_input(&sample);
    assert(mouse_window.sampled==0 && mouse_window.elapsed==0);
    assert(mouse_window.x==0 && mouse_window.y==0);
    mouse_sample=(TheOrderMouse){.dx=1.0f,.dy=1.0f,.captured=1,.sensitivity=1.0f,
        .sens_y=2.0f,.invert_y=1,.buttons=SDL_BUTTON_LMASK};
    mouse_input(&sample);
    assert(sample.right_x>128 && sample.right_y<128);
    assert(128-sample.right_y>sample.right_x-128);
    assert(sample.buttons==BTN_R2 && sample.r2==255);
    mouse_sample=(TheOrderMouse){0};
    sample=(PadData){.right_x=128,.right_y=128,.timestamp=3001000};
    mouse_input(&sample);
    assert(sample.right_x==128 && sample.right_y==128 && sample.buttons==0);
    assert(mouse_window.x==0 && mouse_window.y==0);
}

#ifdef _WIN32
static void write_pad_file(const char *path, const char *tokens, uint64_t ticks) {
    FILE *file=fopen(path,"w");
    assert(file && fputs(tokens,file)>=0);
    assert(fclose(file)==0);
    HANDLE handle=CreateFileA(path,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    assert(handle!=INVALID_HANDLE_VALUE);
    FILETIME stamp={(DWORD)ticks,(DWORD)(ticks>>32)};
    assert(SetFileTime(handle,NULL,NULL,&stamp));
    assert(CloseHandle(handle));
}

static void test_same_second_pad_edits(void) {
    char directory[MAX_PATH], path[MAX_PATH];
    assert(GetTempPathA(sizeof(directory),directory));
    assert(GetTempFileNameA(directory,"pad",0,path));
    FILETIME stamp;
    GetSystemTimeAsFileTime(&stamp);
    uint64_t ticks=((uint64_t)stamp.dwHighDateTime<<32)|stamp.dwLowDateTime;
    ticks-=ticks%10000000;
    assert(_putenv_s("THEORDER_PAD_FILE",path)==0);
    write_pad_file(path,"cross",ticks+10000);
    SDL_Delay(25);
    read_inject();
    uint32_t first=injected.buttons;
    write_pad_file(path,"right",ticks+20000);
    SDL_Delay(25);
    read_inject();
    uint32_t second=injected.buttons;
    write_pad_file(path,"",ticks+30000);
    SDL_Delay(25);
    read_inject();
    uint32_t released=injected.buttons;
    assert(DeleteFileA(path));
    assert(_putenv_s("THEORDER_PAD_FILE","")==0);
    printf("pad edits: first=0x%x second=0x%x released=0x%x\n",first,second,released);
    fflush(stdout);
    assert(first==BTN_CROSS);
    assert(second==BTN_RIGHT);
    assert(released==0);
}
#endif

int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    test_mouse_sample_rate();
#ifdef _WIN32
    test_same_second_pad_edits();
#endif
    /* Keyboard defaults: Enter is Options alone, while Space remains Cross. */
    assert(pressed_by(&key_map,SDL_SCANCODE_RETURN)==BTN_OPTIONS);
    assert(pressed_by(&key_map,SDL_SCANCODE_SPACE)==BTN_CROSS);
    assert(pressed_by(&pad_map,SDL_GAMEPAD_BUTTON_START)==BTN_OPTIONS);

    ButtonMap options=key_map;
    assert(apply_button_map(&options,"options=P",true)==1);
    assert(pressed_by(&options,SDL_SCANCODE_RETURN)==0);
    assert(pressed_by(&options,SDL_SCANCODE_P)==BTN_OPTIONS);

    ButtonMap explicit_options=key_map;
    assert(apply_button_map(&explicit_options,"options=Return",true)==1);
    assert(pressed_by(&explicit_options,SDL_SCANCODE_RETURN)==BTN_OPTIONS);
    assert(pressed_by(&explicit_options,SDL_SCANCODE_SPACE)==BTN_CROSS);

    ButtonMap keys=key_map;
    assert(apply_button_map(&keys,NULL,true)==0 && apply_button_map(&keys,"",true)==0);
    assert(keys.count==key_map.count);

    /* Swap: Q presses Cross, Space presses Triangle; Space no longer presses Cross. */
    assert(apply_button_map(&keys," cross = Q , triangle=Space",true)==2);
    assert(pressed_by(&keys,SDL_SCANCODE_Q)==BTN_CROSS);
    assert(pressed_by(&keys,SDL_SCANCODE_SPACE)==BTN_TRIANGLE);
    assert(pressed_by(&keys,SDL_SCANCODE_E)==BTN_SQUARE);

    /* Names with spaces, any case; the last entry for a button wins. */
    ButtonMap again=key_map;
    assert(apply_button_map(&again,"CIRCLE=left shift,circle=Left Ctrl",true)==2);
    assert(pressed_by(&again,SDL_SCANCODE_LCTRL)==BTN_CIRCLE);
    assert(pressed_by(&again,SDL_SCANCODE_LSHIFT)==0);

    /* An alternative key per button (SpartanPort cbdbf29): every listed name presses it, an
     * invalid one is skipped by itself, a fifth is ignored. */
    ButtonMap alt=key_map;
    assert(apply_button_map(&alt,"cross=Space|G, square=E|NotAKey|H",true)==2);
    assert(pressed_by(&alt,SDL_SCANCODE_SPACE)==BTN_CROSS && pressed_by(&alt,SDL_SCANCODE_G)==BTN_CROSS);
    assert(pressed_by(&alt,SDL_SCANCODE_E)==BTN_SQUARE && pressed_by(&alt,SDL_SCANCODE_H)==BTN_SQUARE);
    ButtonMap all_bad=key_map;
    assert(apply_button_map(&all_bad,"cross=Nope|Nada",true)==0);
    assert(pressed_by(&all_bad,SDL_SCANCODE_SPACE)==BTN_CROSS);
    ButtonMap five=key_map;
    assert(apply_button_map(&five,"cross=M|N|O|T|U",true)==1);
    assert(pressed_by(&five,SDL_SCANCODE_T)==BTN_CROSS && pressed_by(&five,SDL_SCANCODE_U)==0);
    ButtonMap pads=pad_map;
    assert(apply_button_map(&pads,"cross=a|misc1",false)==1);
    assert(pressed_by(&pads,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CROSS && pressed_by(&pads,SDL_GAMEPAD_BUTTON_MISC1)==BTN_CROSS);

    /* Bad entries are skipped and leave the defaults alone. */
    ButtonMap bad=key_map;
    assert(apply_button_map(&bad,"bogus=Q,cross=NotAKey,circle,=Q,square=,,",true)==0);
    assert(bad.count==key_map.count && pressed_by(&bad,SDL_SCANCODE_SPACE)==BTN_CROSS);

    /* Gamepad: Xbox layout swap; 'touchpad' is the left click, Back stays the right click and
     * the real touchpad click keeps its fingers. */
    ButtonMap pad=pad_map;
    assert(apply_button_map(&pad,"cross=b,circle=a,touchpad=misc1",false)==3);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_EAST)==BTN_CROSS);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CIRCLE);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==CLICK_LEFT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_TOUCHPAD)==BTN_TOUCHPAD);
    assert(apply_button_map(&pad,"touchpad_right=guide,touchpad_left=paddle1",false)==2);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_GUIDE)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)==CLICK_LEFT);
    assert(apply_button_map(&pad,"cross=Space",false)==0); /* a key name is no gamepad button */
    /* Keyboard: each side has a key of its own. */
    assert(pressed_by(&key_map,SDL_SCANCODE_TAB)==CLICK_LEFT && pressed_by(&key_map,SDL_SCANCODE_BACKSPACE)==CLICK_RIGHT);
    ButtonMap sides=key_map;
    assert(apply_button_map(&sides,"touchpad_right=Delete",true)==1);
    assert(pressed_by(&sides,SDL_SCANCODE_BACKSPACE)==0 && pressed_by(&sides,SDL_SCANCODE_DELETE)==CLICK_RIGHT);

    /* Validation of mouse_to_stick deadzone bypass and zero-drift */
    int mx = -1, my = -1;
    mouse_to_stick(0.0f, 0.0f, 1.0f, 1.0f, &mx, &my);
    assert(mx == 0 && my == 0);

    /* Very small delta (sub-noise) produces 0 */
    mouse_to_stick(0.00001f, 0.0f, 1.0f, 1.0f, &mx, &my);
    assert(mx == 0 && my == 0);

    /* Delta of 1.0 produces immediate deadzone bypass (magnitude >= 24) */
    mouse_to_stick(1.0f, 0.0f, 1.0f, 1.0f, &mx, &my);
    assert(mx >= 24 && my == 0);

    /* Independent Y sensitivity scales appropriately */
    int mx1 = 0, my1 = 0;
    mouse_to_stick(1.0f, 1.0f, 1.0f, 2.0f, &mx1, &my1);
    assert(my1 > mx1);

    /* Small resting-axis noise is not treated as a held stick after capture. */
    assert(!stick_active(123) && !stick_active(133));
    assert(stick_active(122) && stick_active(134));

    puts("PASS: pad and key maps, mouse deadzone bypass & capture deadzone");
    return 0;
}
