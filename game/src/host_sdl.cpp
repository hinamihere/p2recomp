#include "host.hpp"
#include "savestate.hpp"

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>

namespace p2::host {
namespace {

SDL_Window *g_window{};
SDL_Renderer *g_renderer{};
SDL_Texture *g_texture{};
SDL_GameController *g_pad{};
std::string g_title;
u32 g_keys{};
u32 g_pad_buttons{};
int g_axis_x{}, g_axis_y{};
AudioPull g_pull{};

constexpr u32 SELECT = 0x0001u, START = 0x0008u, UP = 0x0010u, RIGHT = 0x0020u, DOWN = 0x0040u, LEFT = 0x0080u;
constexpr u32 LTRIGGER = 0x0100u, RTRIGGER = 0x0200u, TRIANGLE = 0x1000u, CIRCLE = 0x2000u, CROSS = 0x4000u,
              SQUARE = 0x8000u;

u32 key_to_button(SDL_Keycode key) {
    switch (key) {
    case SDLK_UP: return UP;
    case SDLK_DOWN: return DOWN;
    case SDLK_LEFT: return LEFT;
    case SDLK_RIGHT: return RIGHT;
    case SDLK_z: return CROSS;
    case SDLK_x: return CIRCLE;
    case SDLK_a: return SQUARE;
    case SDLK_s: return TRIANGLE;
    case SDLK_q: return LTRIGGER;
    case SDLK_w: return RTRIGGER;
    case SDLK_RETURN: return START;
    case SDLK_SPACE: return START;
    case SDLK_BACKSPACE: return SELECT;
    case SDLK_RSHIFT: return SELECT;
    default: return 0u;
    }
}

u32 pad_to_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return CROSS;
    case SDL_CONTROLLER_BUTTON_B: return CIRCLE;
    case SDL_CONTROLLER_BUTTON_X: return SQUARE;
    case SDL_CONTROLLER_BUTTON_Y: return TRIANGLE;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return LTRIGGER;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return RTRIGGER;
    case SDL_CONTROLLER_BUTTON_START: return START;
    case SDL_CONTROLLER_BUTTON_BACK: return SELECT;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return RIGHT;
    default: return 0u;
    }
}

void audio_callback(void *, Uint8 *stream, int len) {
    if (g_pull != nullptr) g_pull(reinterpret_cast<s16 *>(stream), len / 4);
    else std::fill(stream, stream + len, 0);
}

} // namespace

bool init(const std::string &title, int scale, bool smooth) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, smooth ? "1" : "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        LOG_E("host", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    g_title = title;
    g_window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480 * scale,
                                272 * scale, SDL_WINDOW_RESIZABLE);
    if (g_window == nullptr) {
        LOG_E("host", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (g_renderer == nullptr) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    SDL_RenderSetLogicalSize(g_renderer, 480, 272);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480, 272);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; }
    }
    return true;
}

void shutdown() {
    SDL_CloseAudio();
    if (g_pad != nullptr) SDL_GameControllerClose(g_pad);
    if (g_texture != nullptr) SDL_DestroyTexture(g_texture);
    if (g_renderer != nullptr) SDL_DestroyRenderer(g_renderer);
    if (g_window != nullptr) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

void present(const u32 *pixels, int width, int height) {
    if (g_texture == nullptr) return;
    SDL_UpdateTexture(g_texture, nullptr, pixels, width * 4);
    SDL_RenderClear(g_renderer);
    SDL_Rect src{0, 0, width, height};
    SDL_RenderCopy(g_renderer, g_texture, &src, nullptr);
    SDL_RenderPresent(g_renderer);
}

bool pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: return false;
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_ESCAPE) return false;
            if (e.key.keysym.sym == SDLK_F11 ||
                (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT) != 0)) {
                const bool full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                SDL_SetWindowFullscreen(g_window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                break;
            }
            if (e.key.keysym.sym == SDLK_F5 || e.key.keysym.sym == SDLK_F8) {
                const int slot = (e.key.keysym.mod & KMOD_SHIFT) != 0 ? 2 : 1;
                if (e.key.keysym.sym == SDLK_F5) savestate_request_save(slot);
                else savestate_request_load(slot);
                break;
            }
            g_keys |= key_to_button(e.key.keysym.sym);
            break;
        case SDL_KEYUP: g_keys &= ~key_to_button(e.key.keysym.sym); break;
        case SDL_CONTROLLERDEVICEADDED:
            if (g_pad == nullptr) g_pad = SDL_GameControllerOpen(e.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN: g_pad_buttons |= pad_to_button(e.cbutton.button); break;
        case SDL_CONTROLLERBUTTONUP: g_pad_buttons &= ~pad_to_button(e.cbutton.button); break;
        case SDL_CONTROLLERAXISMOTION:
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g_axis_x = e.caxis.value;
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g_axis_y = e.caxis.value;
            break;
        default: break;
        }
    }
    return true;
}

bool fast_forward_held() {
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    return keys != nullptr && keys[SDL_SCANCODE_TAB] != 0;
}

u32 buttons() {
    u32 b = g_keys | g_pad_buttons;
    if (g_pad != nullptr) {
        const int lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        const int rt_ = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 16000) b |= LTRIGGER;
        if (rt_ > 16000) b |= RTRIGGER;
    }
    return b;
}

u8 analog_x() { return static_cast<u8>(std::clamp(128 + g_axis_x / 256, 0, 255)); }
u8 analog_y() { return static_cast<u8>(std::clamp(128 + g_axis_y / 256, 0, 255)); }

void set_title_suffix(const std::string &text) {
    if (g_window != nullptr) SDL_SetWindowTitle(g_window, (g_title + " " + text).c_str());
}

bool start_audio(AudioPull pull, int sample_rate) {
    g_pull = pull;
    SDL_AudioSpec want{};
    want.freq = sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    if (SDL_OpenAudio(&want, nullptr) != 0) { // nullptr: SDL converts from our s16 stereo
        LOG_E("host", "SDL_OpenAudio failed: %s", SDL_GetError());
        return false;
    }
    SDL_PauseAudio(0);
    return true;
}

} // namespace p2::host
