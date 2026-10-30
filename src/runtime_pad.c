/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Default keyboard layout (GOW3_KEY_MAP changes the buttons, see apply_button_map):
 *   WASD left stick (move_*), mouse camera (left button R2, right button L2), Space Cross, C Circle,
 *   R Square, Q Triangle, 1 L1, 3 R1, LShift L3 (sprint), F R3, arrow keys d-pad,
 *   Enter Options, Tab left touchpad (photo mode), Backspace right touchpad (pause menu),
 *   Delete held + mouse = finger on the touchpad (the OWL's commands; swipe_* map a key or a
 *   button to one swipe). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
    /* Host-only sources, never shown to the game: a click on one side of the touchpad from a
     * button or key without a touch surface (play_click turns them into a touch and a click). */
    CLICK_LEFT=0x40000000, CLICK_RIGHT=0x80000000,
    /* Host-only too: the left stick's keys (move_*, WASD by default, issues #83/#89) and swipes
     * on the touchpad from a key or button (swipe_*, the OWL's commands, issue #92). */
    MOVE_FORWARD=0x00200000, MOVE_BACK=0x00400000, MOVE_LEFT=0x00800000, MOVE_RIGHT=0x01000000,
    MOVE_MASK=MOVE_FORWARD|MOVE_BACK|MOVE_LEFT|MOVE_RIGHT,
    SWIPE_UP=0x02000000, SWIPE_DOWN=0x04000000, SWIPE_LEFT=0x08000000, SWIPE_RIGHT=0x10000000,
    SWIPE_MASK=SWIPE_UP|SWIPE_DOWN|SWIPE_LEFT|SWIPE_RIGHT,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}

/* A click without a touch surface (Back/View, Tab, Backspace, the pad file) is played like a
 * finger: the touch is reported CLICK_LEAD_US before the button, the button stays at least
 * CLICK_HOLD_US, and the touch lingers CLICK_TAIL_US after the release. God of War III opens
 * photo mode on the release of a left click with the finger still down, and ignores a touch
 * that comes and goes with the button. The side is chosen when the click starts and kept: the
 * right source with L1 held is a left click (photo mode on pads without a touchpad). */
enum { CLICK_LEAD_US=50000, CLICK_HOLD_US=50000, CLICK_TAIL_US=100000 };
static struct { int side; uint64_t pressed, released; } click={-1,0,0};
static void play_click(PadData *d) {
    const uint64_t now=d->timestamp;
    uint32_t sources=d->buttons & (CLICK_LEFT|CLICK_RIGHT);
    d->buttons&=~(CLICK_LEFT|CLICK_RIGHT);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) { /* a replay, or a pad without fingers */
        d->buttons&=~BTN_TOUCHPAD;
        sources|=CLICK_LEFT;
    }
    const int side=(sources & CLICK_LEFT) ? 0 : (sources & CLICK_RIGHT) ? !(d->buttons & BTN_L1) : -1;
    if (side>=0 && (click.side<0 || click.released)) { click.side=side; click.pressed=now; click.released=0; }
    else if (side<0 && click.side>=0 && !click.released) click.released=now;
    if (click.side<0) return;
    const uint64_t down=click.pressed+CLICK_LEAD_US;
    uint64_t up=down+CLICK_HOLD_US;
    if (!click.released) up=UINT64_MAX;
    else if (click.released>up) up=click.released;
    if (now>=up && now-up>=CLICK_TAIL_US) { click.side=-1; return; }
    if (d->touch_count<2) {
        d->touches[d->touch_count]=(PadTouch){.x=click.side ? 1440 : 480,.y=471,.id=d->touch_count};
        ++d->touch_count;
    }
    if (now>=down && now<up) d->buttons|=BTN_TOUCHPAD;
}

/* A swipe source pressed plays one swipe across the touchpad, SWIPE_US long; holding it does not
 * repeat it. */
enum { SWIPE_US=200000 };
static struct { int dir; uint64_t start; uint32_t held; } swipe={-1,0,0};
static void play_swipe(PadData *d) {
    const uint32_t sources=d->buttons & SWIPE_MASK;
    d->buttons&=~SWIPE_MASK;
    const uint32_t pressed=sources & ~swipe.held;
    swipe.held=sources;
    if (pressed && swipe.dir<0) {
        swipe.dir=(pressed & SWIPE_UP) ? 0 : (pressed & SWIPE_DOWN) ? 1 : (pressed & SWIPE_LEFT) ? 2 : 3;
        swipe.start=d->timestamp;
    }
    if (swipe.dir<0) return;
    const uint64_t t=d->timestamp-swipe.start;
    if (t>=SWIPE_US) { swipe.dir=-1; return; }
    if (d->touch_count>=2) return;
    static const int dx[4]={0,0,-1,1}, dy[4]={-1,1,0,0};
    const float f=(float)t/SWIPE_US-0.5f;
    d->touches[d->touch_count]=(PadTouch){.x=(uint16_t)(960+dx[swipe.dir]*f*900.0f),
        .y=(uint16_t)(471+dy[swipe.dir]*f*600.0f),.id=(uint8_t)(0x60+d->touch_count)};
    ++d->touch_count;
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with GOW3_PAD_MAP (gamepad) and GOW3_KEY_MAP (keyboard), e.g.
 * GOW3_KEY_MAP="cross=Q,square=Left Shift" or GOW3_PAD_MAP="cross=b,circle=a": comma-separated
 * <ps4 button>=<SDL key or gamepad button name>. A mapped PS4 button loses its default sources.
 * touchpad_left (alias touchpad) and touchpad_right are the two sides of the touchpad click:
 * photo mode and the pause menu. Back/View (Share on a DualSense) is the right click, and with
 * L1 held the left one. Sticks, triggers, the real touchpad click and the overlay shortcuts are
 * not remapped. */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",CLICK_LEFT}, {"touchpad_left",CLICK_LEFT}, {"touchpad_right",CLICK_RIGHT},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    {"move_forward",MOVE_FORWARD}, {"move_back",MOVE_BACK}, {"move_left",MOVE_LEFT}, {"move_right",MOVE_RIGHT},
    {"swipe_up",SWIPE_UP}, {"swipe_down",SWIPE_DOWN}, {"swipe_left",SWIPE_LEFT}, {"swipe_right",SWIPE_RIGHT},
};
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[48]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,CLICK_RIGHT},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static ButtonMap key_map={{
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_C,BTN_CIRCLE}, {SDL_SCANCODE_R,BTN_SQUARE},
    {SDL_SCANCODE_Q,BTN_TRIANGLE}, {SDL_SCANCODE_1,BTN_L1}, {SDL_SCANCODE_3,BTN_R1},
    {SDL_SCANCODE_LSHIFT,BTN_L3}, {SDL_SCANCODE_F,BTN_R3},
    {SDL_SCANCODE_RETURN,BTN_OPTIONS}, {SDL_SCANCODE_TAB,CLICK_LEFT}, {SDL_SCANCODE_BACKSPACE,CLICK_RIGHT},
    {SDL_SCANCODE_UP,BTN_UP}, {SDL_SCANCODE_DOWN,BTN_DOWN}, {SDL_SCANCODE_LEFT,BTN_LEFT}, {SDL_SCANCODE_RIGHT,BTN_RIGHT},
    {SDL_SCANCODE_W,MOVE_FORWARD}, {SDL_SCANCODE_S,MOVE_BACK}, {SDL_SCANCODE_A,MOVE_LEFT}, {SDL_SCANCODE_D,MOVE_RIGHT},
},19};

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}

/* Keyboard-map sources beyond SDL scancodes: mouse buttons (MOUSE_SOURCE | bit) and wheel steps. */
enum { MOUSE_SOURCE=0x10000, WHEEL_UP_SOURCE=0x20000, WHEEL_DOWN_SOURCE=0x20001 };
static int key_source(const char *name) {
    static const struct { const char *name; int source; } mouse[]={
        {"Mouse Left",MOUSE_SOURCE|1}, {"Mouse Right",MOUSE_SOURCE|2}, {"Mouse Middle",MOUSE_SOURCE|4},
        {"Mouse 4",MOUSE_SOURCE|8}, {"Mouse 5",MOUSE_SOURCE|16},
        {"Wheel Up",WHEEL_UP_SOURCE}, {"Wheel Down",WHEEL_DOWN_SOURCE},
    };
    for (size_t i=0;i<sizeof(mouse)/sizeof(*mouse);++i) if (!SDL_strcasecmp(name,mouse[i].name)) return mouse[i].source;
    return (int)SDL_GetScancodeFromName(name);
}

/* Mouse look (GOW3_MOUSE_LOOK=1, launcher): the mouse turns the camera as the right stick, its
 * buttons fire (left: R2) and aim (right: L2) besides the keys. Sensitivity and invert Y come
 * live from the settings (launcher, in-game menu: gow3gpu_input_settings). The game reads a
 * stick, so the mouse's filtered speed becomes a deflection: past the game's dead zone as soon as
 * it moves, full at GOW3_MOUSE_FULL_SPEED pixels per second (sensitivity 1), along a curve
 * (GOW3_MOUSE_CURVE) that keeps small motions fine. */
static struct { int look; float dead_zone, full_speed, curve; } mouse_cfg;
static float mouse_vx, mouse_vy;
static uint64_t mouse_last_us, wheel_until_us[2];
/* The live controls, refreshed at every pad sample. */
static Gow3InputSettings input={.mouse_sens=1.0f,.mouse_turn_boost=6.0f,.quick_menu=1};
static int free_camera; /* runtime_pad_free_camera */
static float env_float(const char *name, float fallback) {
    const char *v=getenv(name);
    float f=v && *v ? (float)atof(v) : fallback;
    return f>0.0f ? f : fallback;
}
static void load_mouse(void) {
    static int loaded;
    if (loaded) return;
    loaded=1;
    const char *look=getenv("GOW3_MOUSE_LOOK");
    mouse_cfg.look=look && look[0]=='1';
    mouse_cfg.dead_zone=env_float("GOW3_MOUSE_DEAD_ZONE",0.16f);
    mouse_cfg.full_speed=env_float("GOW3_MOUSE_FULL_SPEED",2400.0f);
    mouse_cfg.curve=env_float("GOW3_MOUSE_CURVE",0.65f);
    if (mouse_cfg.dead_zone>0.9f) mouse_cfg.dead_zone=0.9f;
    gow3gpu_mouse_configure(mouse_cfg.look);
    gow3gpu_input_settings(&input);
    if (mouse_cfg.look) printf("Runtime: mouse look on (sensitivity %.2f%s)\n",input.mouse_sens,input.mouse_invert ? ", inverted Y" : "");
}
static uint8_t mouse_axis(float speed) {
    const float s=speed<0 ? -speed : speed;
    if (s<1.0f) return 128;
    float x=SDL_powf(s/mouse_cfg.full_speed,mouse_cfg.curve);
    x=mouse_cfg.dead_zone+(1.0f-mouse_cfg.dead_zone)*(x>1.0f ? 1.0f : x);
    const int v=(int)(128.0f+(speed<0 ? -x : x)*127.5f);
    return (uint8_t)(v<0 ? 0 : v>255 ? 255 : v);
}
/* Direct mouse look (with mouse look on, unless GOW3_MOUSE_DIRECT=0; src/mouse_hook.h): the game's
 * look input routine is wrapped and the mouse's speed is added to its x/y, linear and without the
 * stick's dead zone, curve or limit (x = speed * sensitivity / full speed, so sensitivity 1
 * matches the emulated stick's full deflection at GOW3_MOUSE_FULL_SPEED). The mouse then no
 * longer moves the emulated right stick; the gamepad's stick still goes through the game. Games
 * without that routine keep the emulated stick. */
static volatile int mouse_direct;
/* Speed for direct look (pixels per second, invert applied), smoothed over GOW3_MOUSE_DIRECT_SMOOTH
 * seconds (0.012): raw per-sample motion stutters (samples without a mouse report turn by 0, the
 * next by double) and lets tiny vertical jitter tilt the view; a shorter window than the stick's
 * 25 ms keeps flicks crisp. */
static float mouse_raw_vx, mouse_raw_vy;
/* Last mouse motion, last on-foot limits call (gow3_mouse_look_limits) and its speed factor. */
static volatile uint64_t mouse_moved_us, mouse_limits_us;
static volatile float mouse_speed_boost;
int gow3_mouse_direct_wanted(void) {
    const char *look=getenv("GOW3_MOUSE_LOOK"), *direct=getenv("GOW3_MOUSE_DIRECT");
    return look && look[0]=='1' && !(direct && direct[0]=='0');
}
void gow3_mouse_direct_status(int ready) { mouse_direct=ready; }
static float mouse_direct_axis(const char *name) {
    const char *v=getenv(name);
    return v && *v ? (float)atof(v) : 1.0f;
}
ABI void gow3_mouse_look_adjust(float *x, float *y) {
    /* GOW3_MOUSE_DIRECT_X/_Y scale (and flip, if negative) each axis; 1 by default. The routine's
     * y grows upwards (the stick's byte grows downwards), hence the minus. */
    static float sign_x, sign_y;
    if (!mouse_direct || (mouse_raw_vx==0.0f && mouse_raw_vy==0.0f)) return;
    if (sign_x==0.0f) { sign_x=mouse_direct_axis("GOW3_MOUSE_DIRECT_X"); sign_y=mouse_direct_axis("GOW3_MOUSE_DIRECT_Y"); }
    /* The turn rate is x times a speed of its own (test 2026-10-22: scaling x down by the limits'
     * speed factor slowed the turn by as much), so x keeps the plain gain. */
    const float gain=input.mouse_sens/mouse_cfg.full_speed;
    *x+=mouse_raw_vx*gain*sign_x;
    *y-=mouse_raw_vy*gain*sign_y;
}
/* On foot, the player's turn speeds (limits +0x0c heading, +0x1c pitch) cap how fast the view
 * turns and the accelerations (+0x10, +0x20) ramp a stick up to them: a mouse flick hits both.
 * For 0.3 s after the mouse last moved, speeds are raised by the "fast turns" setting (6x) and
 * accelerations by that times GOW3_MOUSE_DIRECT_ACCEL (50x); a gamepad keeps the game's own. */
ABI void gow3_mouse_look_limits(float *limits) {
    static float accel;
    if (!mouse_direct || now_us()-mouse_moved_us>300000) return;
    if (accel==0.0f) {
        const char *a=getenv("GOW3_MOUSE_DIRECT_ACCEL");
        accel=a && *a && atof(a)>0 ? (float)atof(a) : 50.0f;
    }
    mouse_speed_boost=input.mouse_turn_boost>=1.0f ? input.mouse_turn_boost : 1.0f;
    limits[3]*=mouse_speed_boost;
    limits[7]*=mouse_speed_boost;
    limits[4]*=accel*mouse_speed_boost;
    limits[8]*=accel*mouse_speed_boost;
    mouse_limits_us=now_us();
}

/* Physical stick dead zone (0..0.3, launcher Controls and in-game menu, live): radial, rescaled so
 * the stick still reaches full deflection; worn sticks stop drifting. Off (0) by default: the
 * game keeps its own dead zone after this one. Gamepad sticks only, not mouse look or keys. */
static void stick_axes(SDL_Gamepad *g, SDL_GamepadAxis ax, SDL_GamepadAxis ay, uint8_t *x, uint8_t *y) {
    const float dead_zone=input.pad_dead_zone<0.0f ? 0.0f : input.pad_dead_zone>0.3f ? 0.3f : input.pad_dead_zone;
    int16_t vx=SDL_GetGamepadAxis(g,ax), vy=SDL_GetGamepadAxis(g,ay);
    if (dead_zone>0.0f) {
        const float fx=vx/32767.0f, fy=vy/32767.0f, m=SDL_sqrtf(fx*fx+fy*fy);
        float scale=m<=dead_zone ? 0.0f : (m-dead_zone)/(1.0f-dead_zone)/m;
        if (m*scale>1.0f) scale=1.0f/m;
        vx=(int16_t)SDL_clamp(fx*scale*32767.0f,-32768.0f,32767.0f);
        vy=(int16_t)SDL_clamp(fy*scale*32767.0f,-32768.0f,32767.0f);
    }
    *x=axis(vx); *y=axis(vy);
}
static int source_down(int source, const bool *k, uint32_t mouse, uint64_t now) {
    if (source>=WHEEL_UP_SOURCE) return now<wheel_until_us[source-WHEEL_UP_SOURCE];
    if (source>=MOUSE_SOURCE) return (mouse & (uint32_t)(source & 0xff))!=0;
    return k && source>0 && source<SDL_SCANCODE_COUNT && k[source];
}

/* Applies a GOW3_PAD_MAP/GOW3_KEY_MAP value; returns how many entries were used. Bad entries are
 * logged and skipped, so their PS4 button keeps its default. */
static int apply_button_map(ButtonMap *map, const char *spec, bool keyboard) {
    if (!spec || !*spec) return 0;
    char copy[1024];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=trim(entry), *name=trim(eq+1);
        entry=next;
        uint32_t ps=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(button,ps_names[i].name)) ps=ps_names[i].ps;
        const int source=!*name ? -1 : keyboard ? key_source(name) : (int)SDL_GetGamepadButtonFromString(name);
        if (ps && !*name) { /* "button=": no source at all (its key went to another button) */
            size_t kept=0;
            for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
            map->count=kept;
            ++used;
            continue;
        }
        if (!ps || source<=(keyboard ? (int)SDL_SCANCODE_UNKNOWN : (int)SDL_GAMEPAD_BUTTON_INVALID)) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        size_t kept=0;
        for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
        map->count=kept;
        if (map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){source,ps};
        ++used;
    }
    return used;
}

static void load_button_maps(void) {
    static bool loaded;
    if (loaded) return;
    loaded=true;
    const int pad=apply_button_map(&pad_map,getenv("GOW3_PAD_MAP"),false);
    load_mouse();
    /* Mouse look: the left button fires (R2) and the right one aims (L2), unless the map moves them. */
    if (mouse_cfg.look && key_map.count+2<=sizeof(key_map.entries)/sizeof(*key_map.entries)) {
        key_map.entries[key_map.count++]=(ButtonSource){MOUSE_SOURCE|1,BTN_R2};
        key_map.entries[key_map.count++]=(ButtonSource){MOUSE_SOURCE|2,BTN_L2};
    }
    const int keys=apply_button_map(&key_map,getenv("GOW3_KEY_MAP"),true);
    if (pad || keys) printf("Runtime: pad map: %d gamepad and %d keyboard buttons remapped\n",pad,keys);
}

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}
/* Quick menu (issue #49): holding Select/Back/Share QUICK_MENU_HOLD_US opens the host quick menu
 * (Resume / Settings / Exit game). While the button is down the game does not see it; released
 * sooner, it is played as a short press of its usual PS4 button (the touchpad click). A held press
 * that opened the menu, or a pad lost while holding, never reaches the game. */
enum { QUICK_MENU_HOLD_US=1000000, QUICK_MENU_TAP_US=120000 };
static struct { uint64_t since, tap_until; int opened; } back_hold;
static uint32_t quick_menu_buttons(SDL_Gamepad *g, uint64_t now) {
    uint32_t ps=0;
    for (size_t i=0;i<pad_map.count;++i)
        if (pad_map.entries[i].source==SDL_GAMEPAD_BUTTON_BACK) ps|=pad_map.entries[i].ps;
    if (!g || !input.quick_menu) { back_hold.since=0; back_hold.opened=0; return 0; }
    if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_BACK)) {
        if (!back_hold.since) back_hold.since=now;
        if (!back_hold.opened && now-back_hold.since>=QUICK_MENU_HOLD_US) {
            back_hold.opened=1;
            gow3gpu_quick_menu_open();
        }
    } else if (back_hold.since) {
        if (!back_hold.opened) back_hold.tap_until=now+QUICK_MENU_TAP_US;
        back_hold.since=0;
        back_hold.opened=0;
    }
    return now<back_hold.tap_until ? ps : 0;
}
/* Button prompts (src/prompt_hook.h): the device last used, 1 keyboard/mouse, 2 gamepad (0 none
 * yet: keyboard unless a gamepad is connected), and whether that gamepad is a PlayStation one. */
static int prompt_device;
static int prompt_pad_playstation=1;
static int far_from_center(uint8_t x, uint8_t y) { return x<64 || x>192 || y<64 || y>192; }

/* The icons gameplay prompts show (the menu's Button prompts setting: 0 auto, 1 keyboard, 2 PS4,
 * 3 Xbox): GOW3_PROMPT_SET_KEYBOARD, GOW3_PROMPT_SET_XBOX or -1 for the game's PS4 ones. Auto
 * follows the device last used, and a gamepad's own family. */
int runtime_pad_prompt_set(void) {
    switch (input.prompt_style) {
    case 1: return 0;
    case 2: return -1;
    case 3: return 1;
    default: break;
    }
    const int device=__atomic_load_n(&prompt_device,__ATOMIC_RELAXED);
    if (device ? device==1 : !gamepad) return 0;
    return __atomic_load_n(&prompt_pad_playstation,__ATOMIC_RELAXED) ? -1 : 1;
}

/* The key or mouse button bound to a prompt icon (the game's EIconType, not the pad bits): 1 and
 * an upper-case label of up to 8 characters for a key, 2/3 for the left/right mouse button, 0
 * when it keeps the PS4 icon (unbound, a stick axis, a label the art cannot draw). */
int runtime_pad_prompt_binding(unsigned icon, char *label, size_t size) {
    /* EIconType 9 Select (the touchpad on PS4), 10/11 the stick presses (Left_Analog/Right_Analog). */
    static const uint32_t buttons[16]={BTN_TRIANGLE,BTN_CIRCLE,BTN_CROSS,BTN_SQUARE,
        BTN_L2,BTN_R2,BTN_L1,BTN_R1,BTN_OPTIONS,BTN_TOUCHPAD,BTN_L3,BTN_R3,BTN_UP,BTN_RIGHT,BTN_DOWN,BTN_LEFT};
    if (!label || size<9 || icon>=16 || !buttons[icon]) return 0;
    label[0]=0;
    load_button_maps();
    int source=0;
    for (size_t i=0;i<key_map.count && !source;++i) if (key_map.entries[i].ps==buttons[icon]) source=key_map.entries[i].source;
    /* The keyboard reaches the touchpad as its clicks (Tab, Backspace by default). */
    if (!source && buttons[icon]==BTN_TOUCHPAD)
        for (size_t i=0;i<key_map.count && !source;++i) if (key_map.entries[i].ps==CLICK_LEFT) source=key_map.entries[i].source;
    if (!source && buttons[icon]==BTN_TOUCHPAD)
        for (size_t i=0;i<key_map.count && !source;++i) if (key_map.entries[i].ps==CLICK_RIGHT) source=key_map.entries[i].source;
    if (source==(MOUSE_SOURCE|1)) return 2;
    if (source==(MOUSE_SOURCE|2)) return 3;
    static const struct { int source; const char *label; } mouse[]={
        {MOUSE_SOURCE|4,"MMB"}, {MOUSE_SOURCE|8,"M4"}, {MOUSE_SOURCE|16,"M5"},
        {WHEEL_UP_SOURCE,"WHEEL"}, {WHEEL_DOWN_SOURCE,"WHEEL"}};
    const char *name=NULL;
    for (size_t i=0;i<sizeof(mouse)/sizeof(*mouse);++i) if (source==mouse[i].source) name=mouse[i].label;
    if (!name && source>0 && source<SDL_SCANCODE_COUNT) name=SDL_GetScancodeName((SDL_Scancode)source);
    if (!name || !*name) return 0;
    static const struct { const char *name, *label; } short_names[]={
        {"Left Ctrl","CTRL"}, {"Right Ctrl","CTRL"}, {"Left Shift","SHIFT"}, {"Right Shift","SHIFT"},
        {"Left Alt","ALT"}, {"Right Alt","ALT"}, {"Left GUI","WIN"}, {"Right GUI","WIN"},
        {"Return","ENTER"}, {"Keypad Enter","ENTER"}, {"Space","SPACE"}, {"Backspace","BKSP"},
        {"PageUp","PGUP"}, {"PageDown","PGDN"}, {"CapsLock","CAPS"}, {"Escape","ESC"},
        {"Up","UP"}, {"Down","DOWN"}, {"Left","LEFT"}, {"Right","RIGHT"}, {"Delete","DEL"}, {"Insert","INS"},
    };
    for (size_t i=0;i<sizeof(short_names)/sizeof(*short_names);++i)
        if (!SDL_strcasecmp(name,short_names[i].name)) { name=short_names[i].label; break; }
    const size_t n=strlen(name);
    if (n>8) return 0;
    for (size_t i=0;i<n;++i) {
        char c=name[i];
        if (c>='a' && c<='z') c=(char)(c-'a'+'A');
        if (!((c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='.')) { label[0]=0; return 0; }
        label[i]=c;
    }
    label[n]=0;
    return 1;
}

/* The Xbox button pressing a prompt icon's PS4 button (the pad map, GOW3_PAD_MAP): 4 and its
 * label (A B X Y LB RB LT RT LS RS MENU VIEW UP DOWN LEFT RIGHT), 0 when it keeps the PS4 icon. */
int runtime_pad_prompt_xbox_binding(unsigned icon, char *label, size_t size) {
    static const uint32_t buttons[16]={BTN_TRIANGLE,BTN_CIRCLE,BTN_CROSS,BTN_SQUARE,
        BTN_L2,BTN_R2,BTN_L1,BTN_R1,BTN_OPTIONS,BTN_TOUCHPAD,BTN_L3,BTN_R3,BTN_UP,BTN_RIGHT,BTN_DOWN,BTN_LEFT};
    static const struct { int source; const char *label; } names[]={
        {SDL_GAMEPAD_BUTTON_SOUTH,"A"}, {SDL_GAMEPAD_BUTTON_EAST,"B"}, {SDL_GAMEPAD_BUTTON_WEST,"X"},
        {SDL_GAMEPAD_BUTTON_NORTH,"Y"}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,"LB"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,"RB"},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK,"LS"}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,"RS"}, {SDL_GAMEPAD_BUTTON_START,"MENU"},
        {SDL_GAMEPAD_BUTTON_BACK,"VIEW"}, {SDL_GAMEPAD_BUTTON_TOUCHPAD,"VIEW"},
        {SDL_GAMEPAD_BUTTON_DPAD_UP,"UP"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,"DOWN"},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT,"LEFT"}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,"RIGHT"}};
    if (!label || size<6 || icon>=16) return 0;
    label[0]=0;
    load_button_maps();
    const char *name=NULL;
    /* The triggers always press L2/R2 through their axes. */
    if (buttons[icon]==BTN_L2) name="LT";
    if (buttons[icon]==BTN_R2) name="RT";
    /* The touchpad is clicked with View (its right side) on pads without one. */
    for (int pass=0;pass<3 && !name;++pass) {
        const uint32_t ps=pass==0 ? buttons[icon] : buttons[icon]!=BTN_TOUCHPAD ? 0 : pass==1 ? CLICK_LEFT : CLICK_RIGHT;
        for (size_t i=0;ps && i<pad_map.count && !name;++i) {
            if (pad_map.entries[i].ps!=ps) continue;
            for (size_t j=0;j<sizeof(names)/sizeof(*names);++j)
                if (names[j].source==pad_map.entries[i].source) { name=names[j].label; break; }
        }
    }
    if (!name) return 0;
    memcpy(label,name,strlen(name)+1);
    return 4;
}

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    gow3gpu_input_settings(&input);
    if (gow3gpu_overlay_captures_input()) { /* settings menu open: neutral input */
        if (back_hold.since) back_hold.opened=1; /* the press that opened it stays with the menu */
        return;
    }
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    load_button_maps();
    if (!g) back_hold.since=0, back_hold.opened=0, back_hold.tap_until=0;
    if (g) {
        const uint32_t quick_tap=quick_menu_buttons(g,d->timestamp);
        for (size_t i=0;i<pad_map.count;++i) {
            if (input.quick_menu && pad_map.entries[i].source==SDL_GAMEPAD_BUTTON_BACK) continue;
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        }
        d->buttons|=quick_tap;
        const uint32_t mapped=d->buttons;
        stick_axes(g,SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY,&d->left_x,&d->left_y);
        stick_axes(g,SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY,&d->right_x,&d->right_y);
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        if (d->buttons || d->l2>30 || d->r2>30 || far_from_center(d->left_x,d->left_y) ||
            far_from_center(d->right_x,d->right_y))
            __atomic_store_n(&prompt_device,2,__ATOMIC_RELAXED);
        static SDL_Gamepad *typed;
        if (typed!=g) {
            const SDL_GamepadType type=SDL_GetGamepadType(g);
            __atomic_store_n(&prompt_pad_playstation,type==SDL_GAMEPAD_TYPE_PS3 || type==SDL_GAMEPAD_TYPE_PS4 ||
                             type==SDL_GAMEPAD_TYPE_PS5,__ATOMIC_RELAXED);
            typed=g;
        }
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    /* Free camera: the game gets a neutral pad (the quick menu above still opens) and the mouse
     * is left to runtime_pad_free_camera_input. */
    if (__atomic_load_n(&free_camera,__ATOMIC_RELAXED)) {
        d->buttons=0; d->l2=d->r2=0; d->touch_count=0;
        d->left_x=d->left_y=d->right_x=d->right_y=128;
        mouse_vx=mouse_vy=mouse_raw_vx=mouse_raw_vy=0.0f;
        mouse_last_us=0;
        return;
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. */
    if (!k) return;
    /* Delete held: the mouse is a finger on the touchpad (as in shadPS4), for pads without one
     * (Killzone steers the OWL with swipes). The cursor's place in the game window is the place
     * on the touchpad; each press is a new touch id. */
    static bool mouse_touch_down;
    static uint8_t mouse_touch_id;
    SDL_Window *w=SDL_GetKeyboardFocus();
    if (k[SDL_SCANCODE_DELETE] && w && d->touch_count<2) {
        if (!mouse_touch_down) mouse_touch_id=(uint8_t)((mouse_touch_id+1)&0x7f);
        mouse_touch_down=true;
        float mx=0, my=0;
        int wx=0, wy=0, ww=0, wh=0;
        SDL_GetGlobalMouseState(&mx,&my);
        SDL_GetWindowPosition(w,&wx,&wy);
        SDL_GetWindowSize(w,&ww,&wh);
        if (ww>0 && wh>0)
            d->touches[d->touch_count++]=(PadTouch){.x=touch_axis((mx-wx)/ww,1919),
                .y=touch_axis((my-wy)/wh,942),.id=mouse_touch_id};
    } else {
        mouse_touch_down=false;
    }
    /* Mouse look: motion to the right stick, buttons and wheel to the mapped PS4 buttons. */
    float mdx=0, mdy=0;
    uint32_t mbuttons=0;
    int wheel=0;
    const uint64_t now=now_us();
    const int captured=mouse_cfg.look && gow3gpu_mouse_sample(&mdx,&mdy,&mbuttons,&wheel);
    if (wheel>0) wheel_until_us[0]=now+60000;
    if (wheel<0) wheel_until_us[1]=now+60000;
    int host_used=mdx!=0.0f || mdy!=0.0f;
    for (size_t i=0;i<key_map.count;++i)
        if (source_down(key_map.entries[i].source,k,captured ? mbuttons : 0,now)) {
            d->buttons|=key_map.entries[i].ps;
            host_used=1;
        }
    if (host_used) __atomic_store_n(&prompt_device,1,__ATOMIC_RELAXED);
    if (captured) {
        float dt=mouse_last_us ? (float)(now-mouse_last_us)*1e-6f : 1.0f/60.0f;
        if (dt<1.0f/1000.0f) dt=1.0f/1000.0f;
        if (dt>0.1f) dt=0.1f;
        /* Speed in pixels per second, smoothed over about 25 ms: the game reads the pad once a
         * frame, the mouse reports far more often and unevenly. */
        const float keep=SDL_expf(-dt/0.025f);
        mouse_vx=mouse_vx*keep+(mdx/dt)*(1.0f-keep);
        mouse_vy=mouse_vy*keep+((input.mouse_invert ? -mdy : mdy)/dt)*(1.0f-keep);
        if (mdx==0.0f && SDL_fabsf(mouse_vx)<30.0f) mouse_vx=0.0f;
        if (mdy==0.0f && SDL_fabsf(mouse_vy)<30.0f) mouse_vy=0.0f;
        if (mdx!=0.0f || mdy!=0.0f) mouse_moved_us=now;
        static float direct_smooth=-1.0f;
        if (direct_smooth<0.0f) direct_smooth=env_float("GOW3_MOUSE_DIRECT_SMOOTH",0.012f);
        const float keep_direct=SDL_expf(-dt/direct_smooth);
        mouse_raw_vx=mouse_raw_vx*keep_direct+(mdx/dt)*(1.0f-keep_direct);
        mouse_raw_vy=mouse_raw_vy*keep_direct+((input.mouse_invert ? -mdy : mdy)/dt)*(1.0f-keep_direct);
        if (mdx==0.0f && SDL_fabsf(mouse_raw_vx)<30.0f) mouse_raw_vx=0.0f;
        if (mdy==0.0f && SDL_fabsf(mouse_raw_vy)<30.0f) mouse_raw_vy=0.0f;
        if (!mouse_direct) {
            const uint8_t rx=mouse_axis(mouse_vx*input.mouse_sens), ry=mouse_axis(mouse_vy*input.mouse_sens);
            if (rx!=128) d->right_x=rx;
            if (ry!=128) d->right_y=ry;
        }
    } else {
        mouse_vx=mouse_vy=mouse_raw_vx=mouse_raw_vy=0.0f;
    }
    mouse_last_us=now;
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
}

void runtime_pad_free_camera(int active) { __atomic_store_n(&free_camera,active!=0,__ATOMIC_RELAXED); }

/* Keyboard: WASD move, Q/E down/up, arrows look, Shift faster, Ctrl slower. Mouse: look (the
 * menu's sensitivity and invert). Gamepad: left stick moves, right stick looks, L1/R1 down/up,
 * R2 faster, L2 slower. */
int runtime_pad_free_camera_input(float move[3], float look[4], float *speed, unsigned *keys) {
    static int f9_was_down, f10_was_down;
    memset(move,0,3*sizeof(float)); memset(look,0,4*sizeof(float));
    *speed=4.0f; *keys=0;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k[SDL_SCANCODE_F9], f10=k[SDL_SCANCODE_F10];
    const int allowed=SDL_GetKeyboardFocus()!=NULL && !gow3gpu_overlay_captures_input();
    if (allowed && f9 && !f9_was_down) *keys|=1;
    if (allowed && f10 && !f10_was_down) *keys|=2;
    f9_was_down=f9; f10_was_down=f10;
    if (!__atomic_load_n(&free_camera,__ATOMIC_RELAXED)) return 0; /* the mouse stays the game's */
    float mdx=0, mdy=0;
    uint32_t mbuttons=0;
    int wheel=0;
    const int captured=gow3gpu_mouse_sample(&mdx,&mdy,&mbuttons,&wheel); /* taken even when unused */
    if (!allowed) return 0;
    move[0]=(float)(k[SDL_SCANCODE_D]-k[SDL_SCANCODE_A]);
    move[1]=(float)(k[SDL_SCANCODE_E]-k[SDL_SCANCODE_Q]);
    move[2]=(float)(k[SDL_SCANCODE_W]-k[SDL_SCANCODE_S]);
    look[2]=1.5f*(k[SDL_SCANCODE_RIGHT]-k[SDL_SCANCODE_LEFT]);
    look[3]=1.5f*(k[SDL_SCANCODE_UP]-k[SDL_SCANCODE_DOWN]);
    if (captured) {
        const float gain=0.0025f*input.mouse_sens;
        look[0]=mdx*gain;
        look[1]=(input.mouse_invert ? mdy : -mdy)*gain;
    }
    float fast=k[SDL_SCANCODE_LSHIFT]||k[SDL_SCANCODE_RSHIFT] ? 1.0f : 0.0f;
    float slow=k[SDL_SCANCODE_LCTRL]||k[SDL_SCANCODE_RCTRL] ? 1.0f : 0.0f;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) {
        const float dead=0.15f;
        float a[4];
        static const SDL_GamepadAxis axes[4]={SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY,
                                              SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY};
        for (int i=0;i<4;++i) {
            a[i]=SDL_GetGamepadAxis(g,axes[i])/32767.0f;
            a[i]=a[i]>dead ? (a[i]-dead)/(1.0f-dead) : a[i]<-dead ? (a[i]+dead)/(1.0f-dead) : 0.0f;
        }
        move[0]+=a[0]; move[2]-=a[1];
        move[1]+=(float)(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)-
                         SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
        look[2]+=2.5f*a[2];
        look[3]-=2.5f*(input.mouse_invert ? -a[3] : a[3]);
        fast=SDL_max(fast,SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)/32767.0f);
        slow=SDL_max(slow,SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)/32767.0f);
    }
    pthread_mutex_unlock(&lock);
    *speed=4.0f*(1.0f+3.0f*fast)*(1.0f-0.85f*slow);
    return 1;
}

/* GOW3_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad is the left side; lx= ly= rx= ry= (0..255) override the sticks. An empty file
 * releases everything. */
static struct { uint32_t buttons; int stick[4]; } injected={0,{-1,-1,-1,-1}};
static int replay_armed;      /* 1 while a GOW3_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("GOW3_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* GOW3_PAD_REPLAY */
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* GOW3_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; GOW3_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in GOW3_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("GOW3_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("GOW3_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (gow3gpu_overlay_captures_input()) { hold_after_capture=1; click.side=-1; return; }
    record_sample(d); /* the click sources as they are: a replay plays them again */
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    /* The movement sources (keys, or pad buttons mapped to them) drive the left stick. */
    const uint32_t move=d->buttons & MOVE_MASK;
    d->buttons&=~MOVE_MASK;
    key_axis(&d->left_x,move & MOVE_LEFT,move & MOVE_RIGHT);
    key_axis(&d->left_y,move & MOVE_FORWARD,move & MOVE_BACK);
    play_swipe(d);
    play_click(d);
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    /* The menu's vibration strength scales the game's motors (0: off). */
    const unsigned percent=(unsigned)gow3gpu_vibration_percent();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257u*percent/100u),(uint16_t)(param[1]*257u*percent/100u),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    (void)rgb; return 0;
}
static const RuntimeExport exports[]={
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }

/* probe --gamepad: runs before any game state exists, so it uses its own SDL gamepad and leaves
 * the lock and statics above alone. run.py parses this line; keep the format. */
static const char *label_name(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "a";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "b";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "x";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "triangle";
    default: return "unknown";
    }
}
int runtime_pad_describe(void) {
    SDL_Gamepad *g=NULL;
    const bool ready=SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    /* No event loop yet, so pads show up only after a pump: poll for about 300 ms. */
    for (int waited=0; ready && !g && waited<30; ++waited) {
        SDL_PumpEvents();
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) g=SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        if (!g) SDL_Delay(10);
    }
    if (!g) puts("Gamepad: none");
    else {
        const char *type=SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
        const char *name=SDL_GetGamepadName(g);
        printf("Gamepad: type=%s south=%s east=%s west=%s north=%s name=%s\n",
               type ? type : "unknown",
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_EAST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_WEST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_NORTH)),
               name ? name : "unknown");
        SDL_CloseGamepad(g);
    }
    if (ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return g!=NULL;
}
