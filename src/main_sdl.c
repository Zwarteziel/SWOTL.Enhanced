/* SWOTL Enhanced - SDL3 frontend.
 * Runs the original Secret Weapons of the Luftwaffe (1991) files in a
 * purpose-built PC emulation layer, with modern display scaling, gamepad
 * support and OPL2 music. GPL-3.0-or-later. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "machine.h"
#include "hle.h"
#include "recorder.h"
#include "crt_gl.h"

#define APP_NAME "SWOTL Enhanced"
#include "version.h"
#define INI_NAME "swotl-enhanced.ini"

/* ------------------------------------------------------------------ config */
typedef struct {
    char gamedir[1024];
    char args[128];
    double mips;
    int fullscreen;
    int width, height;
    int scale_mode;      /* 0 nearest, 1 linear, 2 sharp (integer prescale + linear) */
    int aspect;          /* 1 = 4:3 correction */
    int integer_only;    /* integer scaling only */
    int sound;           /* 1 = AdLib */
    int joystick;        /* -1 auto, 0 off, 1 on */
    int detail;
    int advanced;
    int volume;          /* 0..100 */
    float deadzone;
    int kb_flight;       /* 1 = arrow keys drive a smooth virtual stick, 0 = classic keys */
    float kb_ramp, kb_return; /* seconds to full deflection / back to centre */
    float stick_curve;   /* 1 = linear (like the original joystick), higher = calmer centre */
    float stick_range;   /* deflection at full stick, 0.3..1 */
    float mouse_speed;
    double key_rate, key_delay;
    int show_fps;
    int shader;          /* SHADER_* (crt_gl.h) */
    int renderer_sdl;    /* 1 = SDL renderer (no shaders) */
    char pad[32][32];    /* gamepad button mapping strings, by SDL_GamepadButton */
    char pad_lt[32], pad_rt[32];
    int rstick_views;
} Config;

static const char *pad_names[] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "lb", "rb", "dpad_up", "dpad_down", "dpad_left", "dpad_right", NULL};

static void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->mips = 40;
    cfg->width = 1280; cfg->height = 960;
    cfg->scale_mode = 2;
    cfg->shader = SHADER_CRT_AUTO;
    cfg->aspect = 1;
    cfg->sound = 1;
    cfg->joystick = -1;
    cfg->detail = 5;
    cfg->advanced = 0;
    cfg->volume = 80;
    cfg->deadzone = 0.12f;
    cfg->stick_curve = 1.0f;
    cfg->kb_flight = 1;
    cfg->kb_ramp = 0.25f;
    cfg->kb_return = 0.12f;
    cfg->stick_range = 1.0f;
    cfg->mouse_speed = 1.0f;
    cfg->key_rate = 30;
    cfg->key_delay = 250;
    cfg->rstick_views = 1;
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_SOUTH], "joy1");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_EAST], "joy2");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_WEST], "ENTER");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_NORTH], "KP2/KP8");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_BACK], "ESC");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_START], "ALT+P");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER], "F5/F9");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER], "F8/F9");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_DPAD_UP], "F");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_DPAD_DOWN], "L");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_DPAD_LEFT], "M");
    strcpy(cfg->pad[SDL_GAMEPAD_BUTTON_DPAD_RIGHT], "C");
    strcpy(cfg->pad_lt, "MINUS");
    strcpy(cfg->pad_rt, "PLUS");
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int shader_from_str(const char *v) {
    if (!SDL_strcasecmp(v, "crt-auto") || !SDL_strcasecmp(v, "crt") || !strcmp(v, "1")) return SHADER_CRT_AUTO;
    if (!SDL_strcasecmp(v, "crt-1080p")) return SHADER_CRT_1080P;
    if (!SDL_strcasecmp(v, "crt-1440p")) return SHADER_CRT_1440P;
    if (!SDL_strcasecmp(v, "crt-4k")) return SHADER_CRT_4K;
    return SHADER_NONE;
}
static const char *shader_to_str(int sh) {
    switch (sh) {
    case SHADER_CRT_AUTO: return "crt-auto";
    case SHADER_CRT_1080P: return "crt-1080p";
    case SHADER_CRT_1440P: return "crt-1440p";
    case SHADER_CRT_4K: return "crt-4k";
    default: return "none";
    }
}

/* Writes the complete ini file, with an explanation for every setting and the
 * values in cfg. Used for a new ini file, and to bring the ini file of an
 * older version up to date (new settings, current comments). */
static void config_write(const char *path, const Config *c) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "; " APP_NAME " v" APP_VERSION " settings\n"
        "; Lines starting with ; are comments.\n\n"
        "[game]\n"
        "; Folder with the original SWOTL files. Empty = the folder this program is in.\n"
        "gamedir=%s\n"
        "; Maximum emulated CPU speed in million instructions per second.\n"
        "; 20 is roughly a fast 486; higher gives a smoother 3D view. The speed is\n"
        "; lowered automatically when your PC cannot keep up.\n"
        "mips=%g\n"
        "; Ground detail 1-5\n"
        "detail=%d\n"
        "; Advanced flight controls: 0 or 1. The rudder (F5-F9, gamepad LB/RB or\n"
        "; lt/rt=RUDDER) only works with 1.\n"
        "advanced=%d\n"
        "; Joystick: auto (on when a gamepad is connected at start, or with\n"
        "; keyboard_flight=stick), on, off\n"
        "joystick=%s\n"
        "; Music and sound: 1 (or adlib) = AdLib, sb = Sound Blaster, 0 = off.\n"
        "; SWOTL uses only the FM (AdLib) part of a Sound Blaster, so both sound the same.\n"
        "sound=%s\n"
        "; Volume 0-100\n"
        "volume=%d\n"
        "; Flying with the arrow keys: stick = the arrows move a smooth virtual\n"
        "; joystick (responsive, like a gamepad); classic = the arrows go to the game\n"
        "; as key presses, exactly as in 1991 (one control step per key repeat).\n"
        "keyboard_flight=%s\n"
        "; stick mode: seconds to full deflection while held, and back to centre\n"
        "keyboard_stick_ramp=%g\n"
        "keyboard_stick_return=%g\n"
        "; Key repeat while a key is held (classic mode and all other keys). 30 per\n"
        "; second after 250 ms is the fast typematic setting of most 386/486 BIOSes and\n"
        "; what DOSBox gives on Windows; 10.9 and 500 is the slow power-on default.\n"
        "key_repeat_rate=%g\n"
        "key_repeat_delay=%g\n"
        "; Expert: pass your own SWOTL command line instead (e.g. V A 5 Y N Y 16)\n"
        "args=%s\n\n",
        c->gamedir, c->mips, c->detail, c->advanced,
        c->joystick < 0 ? "auto" : (c->joystick ? "on" : "off"),
        c->sound == 2 ? "sb" : (c->sound ? "1" : "0"), c->volume,
        c->kb_flight ? "stick" : "classic", c->kb_ramp, c->kb_return,
        c->key_rate, c->key_delay, c->args);
    fprintf(f,
        "[display]\n"
        "; Start in full screen: 0 or 1 (Alt+Enter or F11 switches)\n"
        "fullscreen=%d\n"
        "; Window size in pixels\n"
        "width=%d\n"
        "height=%d\n"
        "; shader: crt-auto = CRT monitor emulation as in DOSBox Staging 0.83 (picks\n"
        "; the VGA variant that fits the window or screen: 1080p, 1440p or 4K; small\n"
        "; windows get plain 'scale' instead); crt-1080p, crt-1440p, crt-4k = always\n"
        "; that variant; none = no CRT effect. Ctrl+F6 switches between crt-auto and\n"
        "; none. Needs OpenGL 3.3.\n"
        "shader=%s\n"
        "; renderer: opengl (needed for the shader) or sdl (Direct3D on Windows, no shader)\n"
        "renderer=%s\n"
        "; scale (without CRT shader): sharp (crisp pixels, smooth edges), nearest\n"
        "; (blocky), linear (soft)\n"
        "scale=%s\n"
        "; aspect: 1 = original 4:3 monitor shape, 0 = square pixels (16:10)\n"
        "aspect=%d\n"
        "; integer (without CRT shader): 1 = only whole-number scale factors\n"
        "integer=%d\n"
        "; mouse speed multiplier (the mouse is captured when you click in the window)\n"
        "mouse_speed=%g\n"
        "; show speed, frame rate and CPU speed in the picture (Ctrl+F9 toggles);\n"
        "; they are always in the title bar\n"
        "show_fps=%d\n\n",
        c->fullscreen, c->width, c->height, shader_to_str(c->shader),
        c->renderer_sdl ? "sdl" : "opengl",
        c->scale_mode == 0 ? "nearest" : (c->scale_mode == 1 ? "linear" : "sharp"),
        c->aspect, c->integer_only, c->mouse_speed, c->show_fps);
    fprintf(f,
        "[gamepad]\n"
        "; Left stick = joystick. Right stick = look around (numeric keypad views).\n"
        "; Values: joy1..joy4 (joystick buttons) or a key: A-Z, 0-9, F1-F12, KP0-KP9,\n"
        "; ENTER, SPACE, ESC, TAB, PLUS, MINUS, UP, DOWN, LEFT, RIGHT, with optional\n"
        "; ALT+ / CTRL+ / SHIFT+ prefix. KEY1/KEY2 = press KEY1, send KEY2 on release.\n");
    static const int order[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        int b = order[i];
        if (b >= SDL_GAMEPAD_BUTTON_GUIDE && b <= SDL_GAMEPAD_BUTTON_RIGHT_STICK && !c->pad[b][0]) continue;
        fprintf(f, "%s=%s\n", pad_names[b], c->pad[b]);
    }
    fprintf(f,
        "; Triggers. A key (e.g. PLUS): repeated presses while pulled, faster when\n"
        "; pulled further (throttle). KEY1/KEY2: held like a button. RUDDER: analog\n"
        "; rudder, LT left and RT right (needs advanced=1); a slight pull gives a\n"
        "; little rudder. To use the triggers for the rudder and the bumpers for the\n"
        "; throttle: lt=RUDDER, rt=RUDDER, lb=MINUS, rb=PLUS.\n"
        "lt=%s\n"
        "rt=%s\n"
        "; right stick looks around in flight: 0 or 1\n"
        "rightstick_views=%d\n"
        "deadzone=%g\n"
        "; Response curve of the left stick in flight. 1.0 = linear, the same as a\n"
        "; 1991 joystick. Higher values (e.g. 1.5 - 2.5) give finer control around the\n"
        "; centre and still full deflection at the edge. Ctrl+F7 / Ctrl+F8 adjust it\n"
        "; while playing and save it here.\n"
        "stick_curve=%g\n"
        "; Deflection at full stick, 0.3 - 1.0 (lower = gentler maximum input)\n"
        "stick_range=%g\n",
        c->pad_lt, c->pad_rt, c->rstick_views, c->deadzone, c->stick_curve, c->stick_range);
    fclose(f);
}

static void config_load(Config *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { config_write(path, cfg); return; }
    char line[512], section[32] = "";
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') { snprintf(section, sizeof section, "%s", s + 1); char *e = strchr(section, ']'); if (e) *e = 0; continue; }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!strcmp(section, "game")) {
            if (!strcmp(k, "gamedir")) snprintf(cfg->gamedir, sizeof cfg->gamedir, "%s", v);
            else if (!strcmp(k, "mips")) { cfg->mips = atof(v); if (cfg->mips < 1) cfg->mips = 1; if (cfg->mips > 400) cfg->mips = 400; }
            else if (!strcmp(k, "detail")) cfg->detail = atoi(v);
            else if (!strcmp(k, "advanced")) cfg->advanced = atoi(v);
            else if (!strcmp(k, "joystick")) cfg->joystick = !strcmp(v, "on") || !strcmp(v, "1") ? 1 : (!strcmp(v, "off") || !strcmp(v, "0") ? 0 : -1);
            else if (!strcmp(k, "sound")) cfg->sound = !SDL_strcasecmp(v, "sb") || !SDL_strcasecmp(v, "soundblaster") ? 2 : (!SDL_strcasecmp(v, "adlib") ? 1 : atoi(v));
            else if (!strcmp(k, "volume")) cfg->volume = atoi(v);
            else if (!strcmp(k, "args")) snprintf(cfg->args, sizeof cfg->args, "%s", v);
            else if (!strcmp(k, "keyboard_flight")) cfg->kb_flight = !SDL_strcasecmp(v, "classic") || !strcmp(v, "0") ? 0 : 1;
            else if (!strcmp(k, "keyboard_stick_ramp")) { cfg->kb_ramp = (float)atof(v); if (cfg->kb_ramp < 0.05f) cfg->kb_ramp = 0.05f; if (cfg->kb_ramp > 3) cfg->kb_ramp = 3; }
            else if (!strcmp(k, "keyboard_stick_return")) { cfg->kb_return = (float)atof(v); if (cfg->kb_return < 0.02f) cfg->kb_return = 0.02f; if (cfg->kb_return > 3) cfg->kb_return = 3; }
            else if (!strcmp(k, "key_repeat_rate")) { cfg->key_rate = atof(v); if (cfg->key_rate < 2) cfg->key_rate = 2; if (cfg->key_rate > 30) cfg->key_rate = 30; }
            else if (!strcmp(k, "key_repeat_delay")) { cfg->key_delay = atof(v); if (cfg->key_delay < 250) cfg->key_delay = 250; if (cfg->key_delay > 1000) cfg->key_delay = 1000; }
        } else if (!strcmp(section, "display")) {
            if (!strcmp(k, "fullscreen")) cfg->fullscreen = atoi(v);
            else if (!strcmp(k, "width")) cfg->width = atoi(v);
            else if (!strcmp(k, "height")) cfg->height = atoi(v);
            else if (!strcmp(k, "scale")) cfg->scale_mode = !strcmp(v, "nearest") ? 0 : (!strcmp(v, "linear") ? 1 : 2);
            else if (!strcmp(k, "aspect")) cfg->aspect = atoi(v);
            else if (!strcmp(k, "integer")) cfg->integer_only = atoi(v);
            else if (!strcmp(k, "mouse_speed")) cfg->mouse_speed = (float)atof(v);
            else if (!strcmp(k, "show_fps")) cfg->show_fps = atoi(v);
            else if (!strcmp(k, "shader")) cfg->shader = shader_from_str(v);
            else if (!strcmp(k, "renderer")) cfg->renderer_sdl = !SDL_strcasecmp(v, "sdl");
        } else if (!strcmp(section, "gamepad")) {
            if (!strcmp(k, "lt")) snprintf(cfg->pad_lt, sizeof cfg->pad_lt, "%s", v);
            else if (!strcmp(k, "rt")) snprintf(cfg->pad_rt, sizeof cfg->pad_rt, "%s", v);
            else if (!strcmp(k, "deadzone")) cfg->deadzone = (float)atof(v);
            else if (!strcmp(k, "stick_curve")) cfg->stick_curve = (float)atof(v);
            else if (!strcmp(k, "stick_range")) cfg->stick_range = (float)atof(v);
            else if (!strcmp(k, "rightstick_views")) cfg->rstick_views = atoi(v);
            else for (int i = 0; pad_names[i]; i++) if (!strcmp(k, pad_names[i])) snprintf(cfg->pad[i], sizeof cfg->pad[i], "%s", v);
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ keys */
/* SDL scancode -> XT set 1 (bit 8 = E0 extended) */
static int sdl_to_xt(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D; case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F1: return 0x3B; case SDL_SCANCODE_F2: return 0x3C; case SDL_SCANCODE_F3: return 0x3D;
    case SDL_SCANCODE_F4: return 0x3E; case SDL_SCANCODE_F5: return 0x3F; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42; case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44; case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45; case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x11C; case SDL_SCANCODE_RCTRL: return 0x11D; case SDL_SCANCODE_KP_DIVIDE: return 0x135;
    case SDL_SCANCODE_RALT: return 0x138; case SDL_SCANCODE_HOME: return 0x147; case SDL_SCANCODE_UP: return 0x148;
    case SDL_SCANCODE_PAGEUP: return 0x149; case SDL_SCANCODE_LEFT: return 0x14B; case SDL_SCANCODE_RIGHT: return 0x14D;
    case SDL_SCANCODE_END: return 0x14F; case SDL_SCANCODE_DOWN: return 0x150; case SDL_SCANCODE_PAGEDOWN: return 0x151;
    case SDL_SCANCODE_INSERT: return 0x152; case SDL_SCANCODE_DELETE: return 0x153;
    default: return 0;
    }
}

typedef struct { int mods; int key; int release_key; int joy; int release_mods; int rudder; } Action;

static int key_by_name(const char *n) {
    static const struct { const char *name; int code; } t[] = {
        {"ESC", 0x01}, {"ENTER", 0x1C}, {"RETURN", 0x1C}, {"SPACE", 0x39}, {"TAB", 0x0F}, {"BACKSPACE", 0x0E},
        {"MINUS", 0x4A}, {"PLUS", 0x4E}, {"EQUALS", 0x0D}, {"COMMA", 0x33}, {"PERIOD", 0x34}, {"SLASH", 0x35},
        {"UP", 0x148}, {"DOWN", 0x150}, {"LEFT", 0x14B}, {"RIGHT", 0x14D},
        {"KP0", 0x52}, {"KP1", 0x4F}, {"KP2", 0x50}, {"KP3", 0x51}, {"KP4", 0x4B}, {"KP5", 0x4C},
        {"KP6", 0x4D}, {"KP7", 0x47}, {"KP8", 0x48}, {"KP9", 0x49}, {"KPPLUS", 0x4E}, {"KPMINUS", 0x4A},
        {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E}, {"F5", 0x3F}, {"F6", 0x40}, {"F7", 0x41},
        {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44}, {"F11", 0x57}, {"F12", 0x58}, {NULL, 0}};
    for (int i = 0; t[i].name; i++) if (!SDL_strcasecmp(t[i].name, n)) return t[i].code;
    if (strlen(n) == 1) {
        static const char *row = "\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./";
        char ch = (char)tolower((unsigned char)n[0]);
        for (int i = 0; i < 0x35; i++) if (row[i] == ch && ch) return i + 1;
    }
    return 0;
}

static void parse_one(const char *s, int *mods, int *key, int *joy) {
    char buf[32]; snprintf(buf, sizeof buf, "%s", s);
    char *p = buf;
    *mods = 0; *key = 0; *joy = 0;
    for (;;) {
        if (!SDL_strncasecmp(p, "ALT+", 4)) { *mods |= 1; p += 4; }
        else if (!SDL_strncasecmp(p, "CTRL+", 5)) { *mods |= 2; p += 5; }
        else if (!SDL_strncasecmp(p, "SHIFT+", 6)) { *mods |= 4; p += 6; }
        else break;
    }
    if (!SDL_strncasecmp(p, "joy", 3)) { *joy = atoi(p + 3); return; }
    *key = key_by_name(p);
}

static Action parse_action(const char *s) {
    Action a; memset(&a, 0, sizeof a);
    if (!SDL_strcasecmp(s, "RUDDER")) { a.rudder = 1; return a; } /* analog rudder (triggers) */
    char buf[32]; snprintf(buf, sizeof buf, "%s", s);
    char *slash = strchr(buf, '/');
    if (slash && slash != buf) {
        *slash = 0;
        int j; parse_one(slash + 1, &a.release_mods, &a.release_key, &j);
    }
    parse_one(buf, &a.mods, &a.key, &a.joy);
    return a;
}

/* ------------------------------------------------------------------ app */
typedef struct {
    Config cfg;
    Machine *m;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex, *sharp;
    int tex_w, tex_h, sharp_k;
    SDL_AudioStream *audio;
    SDL_Gamepad *pad;
    Action pad_act[32], lt_act, rt_act;
    int joy_buttons;
    uint8_t pad_route[32]; /* where a held pad button went: 0 none, 1 mouse, 2 its action */
    int view_dir;          /* current right-stick view key */
    double lt_next, rt_next;
    int lt_down, rt_down;
    int rudder_state;      /* rudder sent to the game: -2 hard left .. 0 centre .. 2 hard right */
    int rudder_hint_shown;
    SDL_FRect dst;
    uint32_t *frame;
    int paused_focus;
    char base[1024];
    char status[128];
    Uint64 status_until;
    int captured;
    float mouse_fx, mouse_fy;
    double fps;            /* 3D frames per second, -1 = not in 3D */
    char ini_path[1200];
    int game_joy;          /* game was started with joystick enabled */
    Recorder rec; int recording; Uint64 rec_start, rec_next; char rec_name[64];
    char args[128];
    int kb_held[4], kb_taken[4]; /* left, right, up, down */
    float kb_x, kb_y;
    float pad_x, pad_y; int pad_ok;
    uint64_t fps_frames;
    double tempo;          /* emulated time / real time over the last second */
    double tempo_emu0;
    Uint64 fps_t0;
    CrtGL *gl;             /* OpenGL presentation (CRT shaders); NULL = SDL renderer */
    const char *shader_name;
    int frames_presented;
    char gl_error[512];
} App;

static void send_key(App *a, int code, int down) {
    if (!code) return;
    machine_key(a->m, code & 0xFF, (code & 0x100) != 0, down);
}
static void send_mods(App *a, int mods, int down) {
    if (mods & 2) send_key(a, 0x1D, down);
    if (mods & 1) send_key(a, 0x38, down);
    if (mods & 4) send_key(a, 0x2A, down);
}
static void tap(App *a, int mods, int key) {
    send_mods(a, mods, 1);
    send_key(a, key, 1);
    send_key(a, key, 0);
    send_mods(a, mods, 0);
}

static void do_action(App *a, Action *act, int down) {
    if (act->joy) {
        int bit = 1 << (act->joy - 1);
        if (down) a->joy_buttons |= bit; else a->joy_buttons &= ~bit;
        return;
    }
    if (down) {
        send_mods(a, act->mods, 1);
        send_key(a, act->key, 1);
        if (act->release_key) { send_key(a, act->key, 0); send_mods(a, act->mods, 0); }
    } else {
        if (act->release_key) tap(a, act->release_mods, act->release_key);
        else { send_key(a, act->key, 0); send_mods(a, act->mods, 0); }
    }
}

static void update_layout(App *a) {
    if (a->gl) return; /* computed for every frame in crtgl_frame */
    int ww, wh;
    SDL_GetRenderOutputSize(a->ren, &ww, &wh);
    double src_w = 320, src_h = a->cfg.aspect ? 240 : 200;
    if (a->tex_w == 640 && a->tex_h == 200) { src_w = 640; src_h = a->cfg.aspect ? 480 : 400; }
    double s = SDL_min(ww / src_w, wh / src_h);
    if (a->cfg.integer_only && s >= 1) s = SDL_floor(s);
    a->dst.w = (float)(src_w * s); a->dst.h = (float)(src_h * s);
    a->dst.x = (float)((ww - a->dst.w) / 2); a->dst.y = (float)((wh - a->dst.h) / 2);
}

static void update_title(App *a) {
    char title[256];
    int n = snprintf(title, sizeof title, APP_NAME " v" APP_VERSION);
    if (a->tempo > 0) n += snprintf(title + n, sizeof title - n, "  |  speed %.0f%%", a->tempo * 100);
    if (a->fps >= 0) n += snprintf(title + n, sizeof title - n, "  |  fps %.0f", a->fps);
    else n += snprintf(title + n, sizeof title - n, "  |  fps -");
    n += snprintf(title + n, sizeof title - n, "  |  CPU %.0f MIPS", a->m ? a->m->ips : a->cfg.mips);
    if (a->status[0]) snprintf(title + n, sizeof title - n, "  |  %s", a->status);
    SDL_SetWindowTitle(a->win, title);
}

static void set_status(App *a, const char *msg) {
    snprintf(a->status, sizeof a->status, "%s", msg);
    a->status_until = SDL_GetTicks() + 2500;
    update_title(a);
}

static void set_capture(App *a, int on) {
    if (on == a->captured) return;
    a->captured = on;
    SDL_SetWindowRelativeMouseMode(a->win, on);
    if (on) set_status(a, "Mouse captured - Ctrl+F10 releases it");
}

/* a text box like SDL_RenderDebugText at 2x, for the OpenGL path */
static void gl_text_box(App *a, int x, int y, const char *txt, uint32_t col) {
    static uint32_t buf[(160 * 8 + 8) * 14];
    int n = (int)strlen(txt);
    if (n > 160) n = 160;
    int bw = n * 8 + 8, bh = 14;
    const uint8_t *font = machine_font8x8();
    for (int i = 0; i < bw * bh; i++) buf[i] = 0xC8000000u; /* black, alpha 200 */
    for (int c = 0; c < n; c++)
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++)
                if (font[(uint8_t)txt[c] * 8 + yy] & (0x80 >> xx)) buf[(3 + yy) * bw + 4 + c * 8 + xx] = col;
    crtgl_overlay(a->gl, buf, bw, bh, x * 2, y * 2, 2);
}

static void present_gl(App *a, int w, int h) {
    const char *nm = crtgl_frame(a->gl, a->frame, w, h, a->cfg.shader, a->cfg.scale_mode,
                                 a->cfg.aspect, a->cfg.integer_only, &a->dst);
    if (nm[0] && nm != a->shader_name) {
        if (a->shader_name && a->cfg.shader != SHADER_NONE) {
            char msg[96]; snprintf(msg, sizeof msg, "Display: %s", nm);
            set_status(a, msg);
        }
        a->shader_name = nm;
    }
    if (a->status[0] && SDL_GetTicks() < a->status_until) {
        gl_text_box(a, 4, 4, a->status, 0xFF78E6FFu);
    } else if (a->status[0]) {
        a->status[0] = 0;
        update_title(a);
    }
    if (a->cfg.show_fps) {
        char txt[96];
        if (a->fps >= 0) snprintf(txt, sizeof txt, "speed %.0f%%  fps %.0f  CPU %.0f MIPS", a->tempo * 100, a->fps, a->m->ips);
        else snprintf(txt, sizeof txt, "speed %.0f%%  fps -  CPU %.0f MIPS", a->tempo * 100, a->m->ips);
        int ww, wh; crtgl_output_size(a->gl, &ww, &wh);
        int x = ww / 2 - (int)strlen(txt) * 8 - 8;
        gl_text_box(a, x, 4, txt, 0xFFA0FFA0u);
    }
    a->frames_presented++;
    const char *gs = getenv("SWOTL_DEBUG_GLSHOT");
    if (gs && a->frames_presented > 3 && a->m->time_us >= atof(gs) * 1e6) { /* emulated seconds */
        /* test hook: save the window contents once */
        static int done;
        if (!done) {
            int ww, wh; crtgl_output_size(a->gl, &ww, &wh);
            uint8_t *px = malloc((size_t)ww * wh * 4);
            crtgl_read(a->gl, px, ww, wh);
            char fn[1200]; snprintf(fn, sizeof fn, "%sglshot.ppm", a->base);
            FILE *f = fopen(fn, "wb");
            if (f) {
                fprintf(f, "P6 %d %d 255\n", ww, wh);
                for (int i = 0; i < ww * wh; i++) fwrite(px + i * 4, 1, 3, f);
                fclose(f);
            }
            free(px);
            fprintf(stderr, "[glshot] %dx%d %s (%s)\n", ww, wh, nm, crtgl_info(a->gl));
            done = 1;
        }
    }
    crtgl_swap(a->gl);
}

static void present(App *a) {
    int w, h;
    if (!machine_render(a->m, a->frame, &w, &h)) return;
    if (a->gl) { present_gl(a, w, h); return; }
    if (!a->tex || w != a->tex_w || h != a->tex_h) {
        if (a->tex) SDL_DestroyTexture(a->tex);
        if (a->sharp) { SDL_DestroyTexture(a->sharp); a->sharp = NULL; }
        a->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(a->tex, a->cfg.scale_mode == 1 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        a->tex_w = w; a->tex_h = h;
        update_layout(a);
    }
    SDL_UpdateTexture(a->tex, NULL, a->frame, w * 4);
    SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 255);
    SDL_RenderClear(a->ren);
    if (a->cfg.scale_mode == 2) {
        /* sharp: integer prescale with nearest, then linear to the final size */
        int k = (int)SDL_ceil(SDL_max(a->dst.w / w, a->dst.h / h));
        if (k < 1) k = 1;
        if (k > 8) k = 8;
        if (!a->sharp || a->sharp_k != k) {
            if (a->sharp) SDL_DestroyTexture(a->sharp);
            a->sharp = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w * k, h * k);
            SDL_SetTextureScaleMode(a->sharp, SDL_SCALEMODE_LINEAR);
            a->sharp_k = k;
        }
        SDL_SetRenderTarget(a->ren, a->sharp);
        SDL_RenderTexture(a->ren, a->tex, NULL, NULL);
        SDL_SetRenderTarget(a->ren, NULL);
        SDL_RenderTexture(a->ren, a->sharp, NULL, &a->dst);
    } else {
        SDL_RenderTexture(a->ren, a->tex, NULL, &a->dst);
    }
    if (a->status[0] && SDL_GetTicks() < a->status_until) {
        SDL_SetRenderScale(a->ren, 2, 2);
        SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 200);
        SDL_FRect r = {4, 4, (float)(strlen(a->status) * 8 + 8), 14};
        SDL_RenderFillRect(a->ren, &r);
        SDL_SetRenderDrawColor(a->ren, 255, 230, 120, 255);
        SDL_RenderDebugText(a->ren, 8, 7, a->status);
        SDL_SetRenderScale(a->ren, 1, 1);
    } else if (a->status[0]) {
        a->status[0] = 0;
        update_title(a);
    }
    if (a->cfg.show_fps) {
        char txt[64];
        if (a->fps >= 0) snprintf(txt, sizeof txt, "speed %.0f%%  fps %.0f  CPU %.0f MIPS", a->tempo * 100, a->fps, a->m->ips);
        else snprintf(txt, sizeof txt, "speed %.0f%%  fps -  CPU %.0f MIPS", a->tempo * 100, a->m->ips);
        int ww, wh; SDL_GetRenderOutputSize(a->ren, &ww, &wh);
        float x = ww / 2.0f - strlen(txt) * 8 - 8;
        SDL_SetRenderScale(a->ren, 2, 2);
        SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 200);
        SDL_FRect r = {x, 4, (float)(strlen(txt) * 8 + 8), 14};
        SDL_RenderFillRect(a->ren, &r);
        SDL_SetRenderDrawColor(a->ren, 160, 255, 160, 255);
        SDL_RenderDebugText(a->ren, x + 4, 7, txt);
        SDL_SetRenderScale(a->ren, 1, 1);
    }
    SDL_RenderPresent(a->ren);
}

/* ---- diagnostic recording (Ctrl+F5) ---- */
static void rec_text(uint32_t *buf, int x, int y, const char *t, uint32_t col) {
    const uint8_t *font = machine_font8x8();
    for (; *t; t++, x += 6)
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 6; xx++) {
                int px = x + xx, py = y + yy;
                if (px < 0 || px >= 320 || py < 0 || py >= 200) continue;
                if (font[(uint8_t)*t * 8 + yy] & (0x80 >> xx)) buf[py * 320 + px] = col;
                else if (yy < 8) buf[py * 320 + px] = 0xFF000000u;
            }
}

static void rec_toggle(App *a) {
    if (a->recording) {
        rec_close(&a->rec);
        a->recording = 0;
        char msg[128]; snprintf(msg, sizeof msg, "Recording saved: %s.avi / .log", a->rec_name);
        set_status(a, msg);
        return;
    }
    char avi[1300], logp[1300];
    for (int i = 1; i < 10000; i++) {
        snprintf(a->rec_name, sizeof a->rec_name, "swotl-rec-%04d", i);
        snprintf(avi, sizeof avi, "%s%s.avi", a->base, a->rec_name);
        FILE *f = fopen(avi, "rb");
        if (f) { fclose(f); continue; }
        break;
    }
    snprintf(logp, sizeof logp, "%s%s.log", a->base, a->rec_name);
    if (rec_open(&a->rec, avi, logp, 320, 200, 30)) { set_status(a, "Could not start recording"); return; }
    a->recording = 1;
    a->rec_start = a->rec_next = SDL_GetTicks();
    if (a->rec.log) {
        fprintf(a->rec.log, "SWOTL Enhanced v" APP_VERSION " diagnostic recording\n");
        fprintf(a->rec.log, "args=%s renderer=%s video=%s shader=%s display=%s\n", a->args, a->gl ? crtgl_info(a->gl) : SDL_GetRendererName(a->ren), SDL_GetCurrentVideoDriver(),
                shader_to_str(a->cfg.shader), a->shader_name ? a->shader_name : "-");
        fprintf(a->rec.log, "keyboard_flight=%d ramp=%.3f return=%.3f key_rate=%.1f key_delay=%.0f mips=%.1f joystick_cfg=%d game_joy=%d pad=%s\n",
                a->cfg.kb_flight, a->cfg.kb_ramp, a->cfg.kb_return, a->cfg.key_rate, a->cfg.key_delay, a->cfg.mips, a->cfg.joystick, a->game_joy,
                a->pad ? SDL_GetGamepadName(a->pad) : "none");
        fprintf(a->rec.log, "frame real_ms emu_ms ips fps tempo in3d mouse kbLRUD kb_x kb_y pad_ok pad_x pad_y joy_x joy_y joy_btn joy_polls\n");
    }
    set_status(a, "Recording (Ctrl+F5 stops, max 60 s)");
}

static void rec_tick(App *a) {
    if (!a->recording) return;
    Uint64 now = SDL_GetTicks();
    if (now - a->rec_start > 60000 || a->rec.frames >= 60 * 30) { rec_toggle(a); return; }
    while (now >= a->rec_next && a->rec.frames < 60 * 30) {
        static uint32_t buf[640 * 480], out[320 * 200];
        int w, h;
        machine_render(a->m, buf, &w, &h);
        for (int y = 0; y < 200; y++)
            for (int x = 0; x < 320; x++)
                out[y * 320 + x] = buf[(y * h / 200) * w + x * w / 320];
        char txt[96];
        snprintf(txt, sizeof txt, "%c%c%c%c x%+.2f y%+.2f %s %.0f%%",
                 a->kb_held[0] ? 'L' : '-', a->kb_held[1] ? 'R' : '-', a->kb_held[2] ? 'U' : '-', a->kb_held[3] ? 'D' : '-',
                 a->m->joy_axis[0], a->m->joy_axis[1], machine_in_3d(a->m) ? "3D" : "menu", a->tempo * 100);
        rec_text(out, 2, 2, txt, 0xFF40FF40u);
        rec_frame(&a->rec, out);
        if (a->rec.log)
            fprintf(a->rec.log, "%u %llu %.0f %.1f %.1f %.2f %d %d %d%d%d%d %.2f %.2f %d %.2f %.2f %.2f %.2f %d %llu\n",
                    a->rec.frames - 1, (unsigned long long)(now - a->rec_start), a->m->time_us / 1000.0, a->m->ips, a->fps, a->tempo,
                    machine_in_3d(a->m), machine_mouse_active(a->m), a->kb_held[0], a->kb_held[1], a->kb_held[2], a->kb_held[3],
                    a->kb_x, a->kb_y, a->pad_ok, a->pad_x, a->pad_y, a->m->joy_axis[0], a->m->joy_axis[1], a->m->joy_buttons,
                    (unsigned long long)a->m->joy_triggers);
        a->rec_next += 33;
        /* after a stall, continue from now instead of writing a burst of frames */
        if (a->rec_next + 500 < now) a->rec_next = now + 33;
    }
}

static void screenshot(App *a) {
    int w, h;
    machine_render(a->m, a->frame, &w, &h);
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, a->frame, w * 4);
    if (!s) return;
    char fn[1200];
    for (int i = 1; i < 10000; i++) {
        snprintf(fn, sizeof fn, "%sswotl%04d.bmp", a->base, i);
        FILE *f = fopen(fn, "rb");
        if (f) { fclose(f); continue; }
        break;
    }
    SDL_SaveBMP(s, fn);
    SDL_DestroySurface(s);
    set_status(a, "Screenshot saved");
}

static void open_first_pad(App *a) {
    if (a->pad) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) {
        a->pad = SDL_OpenGamepad(ids[0]);
        if (a->pad) {
            char msg[128];
            snprintf(msg, sizeof msg, "Gamepad: %s", SDL_GetGamepadName(a->pad));
            set_status(a, msg);
        }
    }
    SDL_free(ids);
}

static float axis(App *a, SDL_GamepadAxis ax) {
    float v = SDL_GetGamepadAxis(a->pad, ax) / 32767.0f;
    if (v > 1) v = 1; if (v < -1) v = -1;
    float dz = a->cfg.deadzone;
    if (SDL_fabsf(v) < dz) return 0;
    return (v > 0 ? 1 : -1) * (SDL_fabsf(v) - dz) / (1 - dz);
}

/* response curve for the flight stick: applied to the distance from the
 * centre so diagonal moves keep their direction */
static void stick_shape(App *a, float *x, float *y) {
    float r = SDL_sqrtf(*x * *x + *y * *y);
    if (r < 1e-6f) return;
    float rc = r > 1 ? 1 : r;
    float shaped = SDL_powf(rc, a->cfg.stick_curve) * a->cfg.stick_range;
    *x = *x / r * shaped * (r > 1 ? r : 1);
    *y = *y / r * shaped * (r > 1 ? r : 1);
    if (*x > 1) *x = 1; if (*x < -1) *x = -1;
    if (*y > 1) *y = 1; if (*y < -1) *y = -1;
}

/* change one key in the ini file, keeping everything else */
static void ini_set(const char *path, const char *section, const char *key, const char *value) {
    FILE *f = fopen(path, "r");
    static char buf[65536]; size_t n = 0;
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = 0;
    static char out[70000]; size_t o = 0;
    char cur[64] = ""; int done = 0, in_sec = 0;
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line + 1) : strlen(line);
        char tmp[512]; size_t tl = len < sizeof tmp - 1 ? len : sizeof tmp - 1;
        memcpy(tmp, line, tl); tmp[tl] = 0;
        char *t = trim(tmp);
        if (*t == '[') {
            if (in_sec && !done) { o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\n", key, value); done = 1; }
            snprintf(cur, sizeof cur, "%s", t + 1); char *e = strchr(cur, ']'); if (e) *e = 0;
            in_sec = !SDL_strcasecmp(cur, section);
        } else if (in_sec && !done) {
            char *eq = strchr(t, '=');
            if (eq && *t != ';') {
                *eq = 0;
                if (!SDL_strcasecmp(trim(t), key)) {
                    o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\n", key, value); done = 1;
                    line += len; continue;
                }
            }
        }
        if (o + len < sizeof out) { memcpy(out + o, line, len); o += len; }
        line += len;
    }
    if (!done) {
        if (!in_sec) o += (size_t)snprintf(out + o, sizeof out - o, "%s[%s]\n", (o && out[o - 1] != '\n') ? "\n" : "", section);
        o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\n", key, value);
    }
    f = fopen(path, "w");
    if (f) { fwrite(out, 1, o, f); fclose(f); }
}

static void rudder_hint(App *a);

/* A and B click the mouse in menus and on the map, and are the fire buttons in
 * flight. A release must go where the press went: leaving the map with A used
 * to leave the mouse button held (or the joystick button), and the aircraft
 * kept firing back in the cockpit. */
static void pad_button(App *a, int btn, int down) {
    if (btn < 0 || btn >= 32) return;
    int route;
    if (down) {
        if (a->pad_route[btn]) return; /* already held */
        route = (!machine_in_3d(a->m) && (btn == SDL_GAMEPAD_BUTTON_SOUTH || btn == SDL_GAMEPAD_BUTTON_EAST)) ? 1 : 2;
        a->pad_route[btn] = (uint8_t)route;
    } else {
        route = a->pad_route[btn];
        a->pad_route[btn] = 0;
        if (!route) return;
    }
    if (route == 1) {
        int bit = btn == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
        int b = a->m->mouse_buttons;
        if (down) b |= bit; else b &= ~bit;
        machine_mouse_buttons(a->m, b);
    } else {
        int k = a->pad_act[btn].key;
        if (down && k >= 0x3F && k <= 0x43 && machine_in_3d(a->m)) rudder_hint(a);
        do_action(a, &a->pad_act[btn], down);
    }
}

/* SWOTL only has rudder keys, and only with advanced flight controls:
 * F5 hard left, F6 partial left, F9 centre, F7 partial right, F8 hard right.
 * Measured in a P-51B over 6 s: partial rudder turns about 4 degrees, hard
 * rudder about 76. An analog trigger is therefore turned into key presses:
 * a slight pull gives partial rudder, further pulls switch between centre
 * and hard rudder ten times a second with a duty cycle that follows the
 * trigger (25% gives about 7 degrees, 50% about 23), and a full pull gives
 * hard rudder. */
static int game_advanced(App *a) {
    if (a->cfg.args[0]) {
        char t[8][16]; int n = sscanf(a->cfg.args, "%15s %15s %15s %15s %15s", t[0], t[1], t[2], t[3], t[4]);
        return n >= 5 && (t[4][0] == 'Y' || t[4][0] == 'y');
    }
    return a->cfg.advanced;
}

static void rudder_hint(App *a) {
    if (game_advanced(a) || a->rudder_hint_shown) return;
    a->rudder_hint_shown = 1;
    set_status(a, "Rudder needs advanced=1 in swotl-enhanced.ini");
}

static void update_rudder(App *a, float r, double now_ms, int menu) {
    if (menu) return; /* not in flight: leave the rudder as it is */
    const double period = 100.0;
    float s = r < 0 ? -r : r;
    int dir = r < 0 ? -1 : 1, want;
    if (s < 0.08f) want = 0;
    else if (s < 0.25f) want = 1;
    else if (s > 0.95f) want = 2;
    else want = SDL_fmod(now_ms, period) < s * period ? 2 : 0;
    int state = want * dir;
    if (state == a->rudder_state) return;
    if (state) rudder_hint(a);
    /* partial rudder steps from the current position, so centre first */
    if (state == 1 || state == -1) tap(a, 0, 0x43);
    static const int keys[5] = {0x3F, 0x40, 0x43, 0x41, 0x42}; /* F5 F6 F9 F7 F8 */
    tap(a, 0, keys[state + 2]);
    a->rudder_state = state;
}

/* A trigger set to a key: repeated presses while pulled (throttle). Set to
 * KEY1/KEY2, it holds like a button: KEY1 when pulled, KEY2 when let go. */
static void trigger(App *a, Action *act, float v, double *next, int *down, double now_ms) {
    if (act->rudder || (!act->key && !act->joy)) return;
    if (act->release_key || act->joy) {
        int d = v > 0.3f;
        if (d != *down) { *down = d; do_action(a, act, d); }
        return;
    }
    if (v > 0.3f) {
        if (now_ms >= *next) { tap(a, act->mods, act->key); *next = now_ms + 1000.0 / (a->cfg.key_rate * (0.5 + 0.5 * v)); }
    } else *next = 0;
}

/* release everything the gamepad still holds (gamepad unplugged) */
static void pad_release_all(App *a) {
    for (int b = 0; b < 32; b++) if (a->pad_route[b]) pad_button(a, b, 0);
}

static void poll_gamepad(App *a, double now_ms) {
    a->pad_ok = 0;
    if (!a->pad) return;
    float x = axis(a, SDL_GAMEPAD_AXIS_LEFTX), y = axis(a, SDL_GAMEPAD_AXIS_LEFTY);
    int menu = !machine_in_3d(a->m);
    if (menu) {
        /* in menus the left stick drives the mouse pointer */
        a->mouse_fx += x * 4.0f; a->mouse_fy += y * 3.0f;
        int dx = (int)a->mouse_fx, dy = (int)a->mouse_fy;
        a->mouse_fx -= dx; a->mouse_fy -= dy;
        if (dx || dy) machine_mouse_move(a->m, dx, dy);
    } else {
        stick_shape(a, &x, &y);
        a->pad_x = x; a->pad_y = y; a->pad_ok = 1;
    }
    /* triggers: repeated taps while held */
    float lt = SDL_GetGamepadAxis(a->pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
    float rt = SDL_GetGamepadAxis(a->pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
    trigger(a, &a->lt_act, lt, &a->lt_next, &a->lt_down, now_ms);
    trigger(a, &a->rt_act, rt, &a->rt_next, &a->rt_down, now_ms);
    /* analog rudder on the trigger(s) set to RUDDER: LT left, RT right */
    float rud = (a->rt_act.rudder ? rt : 0.0f) - (a->lt_act.rudder ? lt : 0.0f);
    if (a->lt_act.rudder || a->rt_act.rudder) update_rudder(a, rud, now_ms, menu);
    /* right stick: views */
    if (a->cfg.rstick_views && !menu) {
        float rx = axis(a, SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(a, SDL_GAMEPAD_AXIS_RIGHTY);
        int dir = 0x48; /* KP8 forward */
        float mag = SDL_sqrtf(rx * rx + ry * ry);
        if (mag > 0.6f) {
            float ang = SDL_atan2f(-ry, rx) * 180.0f / SDL_PI_F; /* 0 = right, 90 = up */
            if (ang < 0) ang += 360;
            static const int keys[8] = {0x4D, 0x49, 0x4C, 0x47, 0x4B, 0x4F, 0x50, 0x51}; /* R, UR, up=KP5, UL, L, DL, D, DR */
            int sector = (int)((ang + 22.5f) / 45.0f) & 7;
            dir = keys[sector];
        }
        if (dir != a->view_dir) {
            if (a->view_dir) tap(a, 0, dir);
            a->view_dir = dir;
        }
    }
}

/* Arrow keys as a smooth virtual stick: deflection ramps up while a key is
 * held and springs back when released. Combined with the gamepad stick. */
static void update_stick(App *a, double dt) {
    float tx = (float)(a->kb_held[1] - a->kb_held[0]);
    float ty = (float)(a->kb_held[3] - a->kb_held[2]); /* up = forward = nose down */
    float *v[2] = {&a->kb_x, &a->kb_y};
    float t[2] = {tx, ty};
    for (int i = 0; i < 2; i++) {
        float cur = *v[i], tgt = t[i];
        float rate = (tgt != 0 && (cur * tgt >= 0)) ? 1.0f / a->cfg.kb_ramp : 1.0f / a->cfg.kb_return;
        float step = rate * (float)dt;
        if (cur < tgt) { cur += step; if (cur > tgt) cur = tgt; }
        else if (cur > tgt) { cur -= step; if (cur < tgt) cur = tgt; }
        *v[i] = cur;
    }
    float x = a->kb_x, y = a->kb_y;
    if (a->pad_ok) { x += a->pad_x; y += a->pad_y; }
    if (x > 1) x = 1; if (x < -1) x = -1;
    if (y > 1) y = 1; if (y < -1) y = -1;
    if (!machine_in_3d(a->m)) { x = 0; y = 0; }
    machine_joystick(a->m, x, y, -2, -2, a->joy_buttons);
}

static int arrow_index(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_LEFT: return 0;
    case SDL_SCANCODE_RIGHT: return 1;
    case SDL_SCANCODE_UP: return 2;
    case SDL_SCANCODE_DOWN: return 3;
    default: return -1;
    }
}

/* In flight the arrow keys drive the virtual stick instead of reaching the
 * game. Returns 1 if the key was taken. */
static int handle_arrow(App *a, SDL_Scancode sc, int down) {
    int ai = arrow_index(sc);
    if (ai < 0) return 0;
    if (down && a->cfg.kb_flight && a->game_joy && machine_in_3d(a->m)) {
        a->kb_held[ai] = 1; a->kb_taken[ai] = 1; return 1;
    }
    if (!down && a->kb_taken[ai]) { a->kb_held[ai] = 0; a->kb_taken[ai] = 0; return 1; }
    return 0;
}

static void build_args(App *a, char *out, size_t n, int have_pad) {
    if (a->cfg.args[0]) { snprintf(out, n, "%s", a->cfg.args); return; }
    int joy = a->cfg.joystick < 0 ? (have_pad || a->cfg.kb_flight) : a->cfg.joystick;
    a->game_joy = joy;
    int det = a->cfg.detail < 1 ? 1 : (a->cfg.detail > 5 ? 5 : a->cfg.detail);
    snprintf(out, n, "V %c %d %c %c Y 16", a->cfg.sound == 2 ? 'S' : (a->cfg.sound ? 'A' : 'N'), det, joy ? 'Y' : 'N', a->cfg.advanced ? 'Y' : 'N');
}

int main(int argc, char **argv) {
    static App app;
    App *a = &app;
    config_defaults(&a->cfg);
    const char *bp = SDL_GetBasePath();
    snprintf(a->base, sizeof a->base, "%s", bp ? bp : "./");
    char ini[1200];
    snprintf(ini, sizeof ini, "%s%s", a->base, INI_NAME);
    config_load(&a->cfg, ini);
    snprintf(a->ini_path, sizeof a->ini_path, "%s", ini);
    /* An ini file from an older version is rewritten in the current layout:
     * same values, new settings added, every setting with its explanation.
     * The old file is kept as swotl-enhanced.ini.bak. */
    {
        FILE *f = fopen(ini, "r");
        char first[128] = "";
        int exists = f != NULL;
        if (f) { if (!fgets(first, sizeof first, f)) first[0] = 0; fclose(f); }
        const char *want = "; " APP_NAME " v" APP_VERSION " settings";
        size_t fl = strlen(first);
        while (fl && (first[fl - 1] == '\n' || first[fl - 1] == '\r')) first[--fl] = 0;
        if (exists && strcmp(first, want)) {
            char bak[1300]; snprintf(bak, sizeof bak, "%s.bak", ini);
            remove(bak);
            rename(ini, bak);
            config_write(ini, &a->cfg);
        }
    }
    if (a->cfg.stick_curve < 1.0f) a->cfg.stick_curve = 1.0f;
    if (a->cfg.stick_curve > 3.0f) a->cfg.stick_curve = 3.0f;
    if (a->cfg.stick_range < 0.3f) a->cfg.stick_range = 0.3f;
    if (a->cfg.stick_range > 1.0f) a->cfg.stick_range = 1.0f;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fullscreen")) a->cfg.fullscreen = 1;
        else if (!strcmp(argv[i], "--window")) a->cfg.fullscreen = 0;
        else if (!strcmp(argv[i], "--gamedir") && i + 1 < argc) snprintf(a->cfg.gamedir, sizeof a->cfg.gamedir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--mips") && i + 1 < argc) a->cfg.mips = atof(argv[++i]);
    }
    if (!a->cfg.gamedir[0]) snprintf(a->cfg.gamedir, sizeof a->cfg.gamedir, "%s", a->base);
    size_t gl = strlen(a->cfg.gamedir);
    while (gl > 1 && (a->cfg.gamedir[gl - 1] == '/' || a->cfg.gamedir[gl - 1] == '\\')) a->cfg.gamedir[--gl] = 0;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

    /* check game files */
    {
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/SWOTL.EXE", a->cfg.gamedir);
        FILE *f = fopen(probe, "rb");
        if (!f) { snprintf(probe, sizeof probe, "%s/swotl.exe", a->cfg.gamedir); f = fopen(probe, "rb"); }
        if (!f) {
            char msg[1600];
            snprintf(msg, sizeof msg,
                "The original game files were not found in:\n%s\n\n"
                "Put SWOTL-Enhanced.exe in the folder that contains SWOTL.EXE and the "
                "FE, AC, CP, FX and MIS folders, or set gamedir= in %s.", a->cfg.gamedir, INI_NAME);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, msg, NULL);
            return 1;
        }
        fclose(f);
    }

    SDL_WindowFlags wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (!a->cfg.renderer_sdl) {
        /* OpenGL 3.3 for the CRT shaders; fall back to the SDL renderer */
        crtgl_set_attributes();
        a->win = SDL_CreateWindow(APP_NAME " v" APP_VERSION, a->cfg.width, a->cfg.height, wflags | SDL_WINDOW_OPENGL);
        if (a->win) {
            a->gl = crtgl_create(a->win, a->gl_error, sizeof a->gl_error);
            if (!a->gl) { SDL_DestroyWindow(a->win); a->win = NULL; }
        } else {
            snprintf(a->gl_error, sizeof a->gl_error, "no OpenGL window: %s", SDL_GetError());
        }
        char lp[1200]; snprintf(lp, sizeof lp, "%sswotl-enhanced-gl.log", a->base);
        if (!a->gl) {
            FILE *lf = fopen(lp, "w");
            if (lf) { fprintf(lf, APP_NAME " v" APP_VERSION ": OpenGL not available, CRT shader off.\n%s\n", a->gl_error); fclose(lf); }
        } else {
            remove(lp);
        }
    }
    if (!a->win) {
        a->win = SDL_CreateWindow(APP_NAME " v" APP_VERSION, a->cfg.width, a->cfg.height, wflags);
        if (!a->win) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL); return 1; }
        a->ren = SDL_CreateRenderer(a->win, NULL);
        if (!a->ren) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL); return 1; }
        SDL_SetRenderVSync(a->ren, 1);
    }
    if (a->cfg.fullscreen) SDL_SetWindowFullscreen(a->win, true);

    for (int i = 0; pad_names[i]; i++) a->pad_act[i] = parse_action(a->cfg.pad[i]);
    a->lt_act = parse_action(a->cfg.pad_lt);
    a->rt_act = parse_action(a->cfg.pad_rt);
    open_first_pad(a);

    build_args(a, a->args, sizeof a->args, a->pad != NULL);
    a->m = machine_create(a->cfg.gamedir, "SWOTL.EXE", a->args);
    if (!a->m) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, "Could not start SWOTL.EXE", NULL); return 1; }
    a->m->ips = a->cfg.mips;
    a->m->typ_rate_hz = a->cfg.key_rate;
    a->m->typ_delay_us = a->cfg.key_delay * 1000.0;
    a->frame = calloc(640 * 480, 4);
    a->fps = -1;
    a->fps_t0 = SDL_GetTicks();
    update_title(a);

    if (a->cfg.sound) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
        a->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (a->audio) {
            machine_set_audio_rate(a->m, 44100);
            SDL_SetAudioStreamGain(a->audio, a->cfg.volume / 100.0f);
            SDL_ResumeAudioStreamDevice(a->audio);
        }
    }

    if (a->cfg.fullscreen) set_capture(a, 1);
    else set_status(a, "Click in the window to use the mouse (Ctrl+F10 releases it)");
    if (!a->gl && !a->cfg.renderer_sdl && a->cfg.shader != SHADER_NONE)
        set_status(a, "OpenGL 3.3 not available: CRT shader off (see swotl-enhanced-gl.log)");
    Uint64 last = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    int running = 1;
    static int16_t abuf[8192 * 2];
    double load_avg = 0.5;
    double debug_after = getenv("SWOTL_DEBUG_SHOT") ? atof(getenv("SWOTL_DEBUG_SHOT")) : 0;
    while (running && !a->m->exited) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_WINDOW_RESIZED: case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: update_layout(a); break;
            case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
                int down = e.type == SDL_EVENT_KEY_DOWN;
                SDL_Scancode sc = e.key.scancode;
                if (a->recording && a->rec.log)
                    fprintf(a->rec.log, "KEY t=%llu sc=%d (%s) %s repeat=%d mod=%04X in3d=%d\n", (unsigned long long)(SDL_GetTicks() - a->rec_start), (int)sc,
                            SDL_GetScancodeName(sc), down ? "down" : "up", e.key.repeat, e.key.mod, machine_in_3d(a->m));
                if (sc == SDL_SCANCODE_F5 && (e.key.mod & SDL_KMOD_CTRL)) {
                    if (down && !e.key.repeat) { rec_toggle(a); send_key(a, 0x1D, 0); }
                    break;
                }
                if (down && sc == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                    /* Alt+Enter: fullscreen toggle (release Alt in the game) */
                    a->cfg.fullscreen = !a->cfg.fullscreen;
                    SDL_SetWindowFullscreen(a->win, a->cfg.fullscreen);
                    if (a->cfg.fullscreen) set_capture(a, 1);
                    send_key(a, 0x38, 0);
                    break;
                }
                if (sc == SDL_SCANCODE_F11 && !(e.key.mod & SDL_KMOD_CTRL)) {
                    if (down && !e.key.repeat) { a->cfg.fullscreen = !a->cfg.fullscreen; SDL_SetWindowFullscreen(a->win, a->cfg.fullscreen); if (a->cfg.fullscreen) set_capture(a, 1); }
                    break;
                }
                if (sc == SDL_SCANCODE_F12 && !(e.key.mod & SDL_KMOD_CTRL)) { if (down && !e.key.repeat) screenshot(a); break; }
                if (sc == SDL_SCANCODE_F10 && (e.key.mod & SDL_KMOD_CTRL)) {
                    if (down && !e.key.repeat) { set_capture(a, !a->captured); send_key(a, 0x1D, 0); }
                    break;
                }
                if ((sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F12) && (e.key.mod & SDL_KMOD_CTRL)) {
                    if (down) {
                        a->cfg.mips *= sc == SDL_SCANCODE_F12 ? 1.25 : 0.8;
                        if (a->cfg.mips < 2) a->cfg.mips = 2;
                        if (a->cfg.mips > 400) a->cfg.mips = 400;
                        a->m->ips = a->cfg.mips;
                        char msg[64]; snprintf(msg, sizeof msg, "CPU speed limit: %.0f MIPS", a->cfg.mips);
                        set_status(a, msg);
                    }
                    break;
                }
                if ((sc == SDL_SCANCODE_F7 || sc == SDL_SCANCODE_F8) && (e.key.mod & SDL_KMOD_CTRL)) {
                    if (down) {
                        float c = a->cfg.stick_curve + (sc == SDL_SCANCODE_F8 ? 0.25f : -0.25f);
                        if (c < 1.0f) c = 1.0f;
                        if (c > 3.0f) c = 3.0f;
                        a->cfg.stick_curve = c;
                        char v[16], msg[96];
                        snprintf(v, sizeof v, "%.2f", c);
                        ini_set(a->ini_path, "gamepad", "stick_curve", v);
                        snprintf(msg, sizeof msg, "Stick curve %.2f%s (saved)", c, c <= 1.0f ? " = linear, as original" : "");
                        set_status(a, msg);
                        send_key(a, 0x1D, 0);
                    }
                    break;
                }
                if (sc == SDL_SCANCODE_F6 && (e.key.mod & SDL_KMOD_CTRL)) {
                    if (down && !e.key.repeat) {
                        if (!a->gl) set_status(a, "The CRT shader needs OpenGL 3.3 (renderer=opengl)");
                        else {
                            a->cfg.shader = a->cfg.shader == SHADER_NONE ? SHADER_CRT_AUTO : SHADER_NONE;
                            ini_set(a->ini_path, "display", "shader", shader_to_str(a->cfg.shader));
                            a->shader_name = NULL;
                            set_status(a, a->cfg.shader != SHADER_NONE ? "CRT shader on: crt-auto (saved)" : "CRT shader off (saved)");
                        }
                        send_key(a, 0x1D, 0);
                    }
                    break;
                }
                if (sc == SDL_SCANCODE_F9 && (e.key.mod & SDL_KMOD_CTRL)) {
                    if (down && !e.key.repeat) { a->cfg.show_fps = !a->cfg.show_fps; send_key(a, 0x1D, 0); }
                    break;
                }
                if (e.key.repeat) break; /* the emulated BIOS repeats held keys itself */
                if (handle_arrow(a, sc, down)) break;
                int xt = sdl_to_xt(sc);
                if (down && xt >= 0x3F && xt <= 0x43 && machine_in_3d(a->m)) rudder_hint(a);
                if (xt) send_key(a, xt, down);
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: {
                if (!a->captured) break;
                /* The game moves its own cursor by the driver's position deltas
                 * one driver unit per screen pixel), so feed relative motion scaled to the
                 * size of the picture on screen. */
                float scale = SDL_GetWindowPixelDensity(a->win);
                a->mouse_fx += e.motion.xrel * scale * 320.0f / a->dst.w * a->cfg.mouse_speed;
                a->mouse_fy += e.motion.yrel * scale * 200.0f / a->dst.h * a->cfg.mouse_speed;
                int dx = (int)a->mouse_fx, dy = (int)a->mouse_fy;
                a->mouse_fx -= dx; a->mouse_fy -= dy;
                if (dx || dy) machine_mouse_move(a->m, dx, dy);
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST: set_capture(a, 0); memset(a->kb_held, 0, sizeof a->kb_held); memset(a->kb_taken, 0, sizeof a->kb_taken); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
                if (!a->captured) {
                    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) set_capture(a, 1);
                    break;
                }
                int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : (e.button.button == SDL_BUTTON_RIGHT ? 2 : (e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0));
                int b = a->m->mouse_buttons;
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) b |= bit; else b &= ~bit;
                machine_mouse_buttons(a->m, b);
                break;
            }
            case SDL_EVENT_GAMEPAD_ADDED: open_first_pad(a); break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (a->pad && SDL_GetGamepadID(a->pad) == e.gdevice.which) { pad_release_all(a); SDL_CloseGamepad(a->pad); a->pad = NULL; set_status(a, "Gamepad disconnected"); open_first_pad(a); }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                int down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                pad_button(a, e.gbutton.button, down);
                break;
            }
            default: break;
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (now - last) / freq;
        last = now;
        if (dt > 0.25) dt = 0.25; /* catch up after slow frames; skip only real stalls */
        poll_gamepad(a, now * 1000.0 / freq);
        update_stick(a, dt);
        Uint64 t0 = SDL_GetPerformanceCounter();
        machine_run(a->m, dt * 1e6);
        double spent = (SDL_GetPerformanceCounter() - t0) / freq;
        /* adaptive speed: keep emulation below ~80% of the frame budget */
        if (dt > 0.001) {
            double load = spent / dt;
            load_avg = load_avg * 0.9 + load * 0.1;
            if (load_avg > 0.85 && a->m->ips > 4) a->m->ips *= 0.97;
            else if (load_avg < 0.6 && a->m->ips < a->cfg.mips) { a->m->ips *= 1.01; if (a->m->ips > a->cfg.mips) a->m->ips = a->cfg.mips; }
        }
        if (debug_after > 0 && SDL_GetTicks() > (Uint64)(debug_after * 1000)) {
            fprintf(stderr, "[debug] t=%.1fs emulated=%.1fs ips=%.1f load=%.2f mode=%02X\n", SDL_GetTicks() / 1000.0, a->m->time_us / 1e6, a->m->ips, load_avg, a->m->vmode);
            screenshot(a);
            debug_after = 0;
            if (getenv("SWOTL_DEBUG_QUIT")) running = 0;
        }

        if (a->audio) {
            /* Keep roughly 60-200 ms of audio queued. The emulation produces
             * sound at exactly real-time rate, so a small cushion absorbs
             * frame-time jitter; it is refilled with silence only after a
             * real underrun (e.g. after the window was dragged). */
            const int bytes_per_ms = 44100 * 4 / 1000;
            int queued = SDL_GetAudioStreamQueued(a->audio);
            if (queued < 5 * bytes_per_ms) {
                static int16_t silence[44100 / 1000 * 60 * 2];
                SDL_PutAudioStreamData(a->audio, silence, sizeof silence);
                queued += (int)sizeof silence;
            }
            int n;
            while ((n = machine_audio_pull(a->m, abuf, 8192)) > 0) {
                if (queued < 200 * bytes_per_ms) {
                    SDL_PutAudioStreamData(a->audio, abuf, n * 4);
                    queued += n * 4;
                }
            }
        }
        /* 3D frame rate: count the game's own frame-present calls */
        Uint64 tnow = SDL_GetTicks();
        if (tnow - a->fps_t0 >= 1000) {
            uint64_t f = machine_frames(a->m);
            double fps = (f - a->fps_frames) * 1000.0 / (double)(tnow - a->fps_t0);
            a->fps = fps >= 1.0 ? fps : -1;
            a->tempo = (a->m->time_us - a->tempo_emu0) / 1000.0 / (double)(tnow - a->fps_t0);
            a->tempo_emu0 = a->m->time_us;
            a->fps_frames = f; a->fps_t0 = tnow;
            update_title(a);
        }
        if (getenv("SWOTL_DEBUG_REC")) { /* test hook: record from 8 s to 11 s */
            Uint64 tk = SDL_GetTicks();
            if (!a->recording && tk > 8000 && tk < 9000) rec_toggle(a);
            if (a->recording && tk > 11000) rec_toggle(a);
        }
        rec_tick(a);
        present(a);
        /* if vsync is not available, do not spin the CPU */
        double frame = (SDL_GetPerformanceCounter() - now) / freq;
        if (frame < 0.004) SDL_Delay(2);
    }
    if (a->recording) rec_close(&a->rec);
    if (a->gl) crtgl_destroy(a->gl);
    machine_destroy(a->m);
    SDL_Quit();
    return 0;
}
