#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "platform_api.hpp"
#include "vita_profiler.hpp"
#include "platform/retail_asset_store.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#if defined(__VITA__) || defined(__psp2__)
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
extern "C" {
// 280 MiB user heap
unsigned int _newlib_heap_size_user = 280 * 1024 * 1024;
// Allocate 4 MiB for the main thread stack (default is only ~112 KiB)
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
}
#endif

namespace {
bool initialized = false, running = true;
std::string retail_root;
unsigned input_frame = 0;
SDL_GameController* controller = nullptr;

const char* env(const char* name) {
    const auto* p = std::getenv(name);
    return p && *p ? p : nullptr;
}

void read_pad(outrun::ps5::PadSample& sample) {
    using namespace outrun::ps5;
    sample = PadSample{};
#if defined(__VITA__) || defined(__psp2__)
    SceCtrlData ctrl{};
    if (sceCtrlPeekBufferPositive(0, &ctrl, 1) >= 0) {
        sample.connected = true;
        sample.left_x  = ctrl.lx;
        sample.left_y  = ctrl.ly;
        sample.right_x = ctrl.rx;
        sample.right_y = ctrl.ry;
        // Vita has only L and R triggers: map them strictly to L1/R1 (no L2/R2 double-firing)
        sample.l2 = 0;
        sample.r2 = 0;
        const struct { std::uint32_t raw, logical; } mapping[]{
            { SCE_CTRL_CROSS,    Cross },
            { SCE_CTRL_CIRCLE,   Circle },
            { SCE_CTRL_SQUARE,   Square },
            { SCE_CTRL_TRIANGLE, Triangle },
            { SCE_CTRL_START,    Options },
            { SCE_CTRL_SELECT,   Share },
            { SCE_CTRL_LTRIGGER, L1 },
            { SCE_CTRL_RTRIGGER, R1 },
            { SCE_CTRL_UP,       Up },
            { SCE_CTRL_DOWN,     Down },
            { SCE_CTRL_LEFT,     Left },
            { SCE_CTRL_RIGHT,    Right },
        };
        for (auto m : mapping) {
            if (ctrl.buttons & m.raw) sample.buttons |= m.logical;
        }
        return;
    }
#endif
    if (controller && !SDL_GameControllerGetAttached(controller)) {
        SDL_GameControllerClose(controller);
        controller = nullptr;
    }
    if (!controller) {
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                controller = SDL_GameControllerOpen(i);
                if (controller) break;
            }
        }
    }
    if (controller) {
        sample.connected = true;
        auto axis = [&](SDL_GameControllerAxis a) {
            const int v = SDL_GameControllerGetAxis(controller, a);
            return std::uint8_t(v < 0 ? 128 + v * 128 / 32768 : 128 + v * 127 / 32767);
        };
        sample.left_x  = axis(SDL_CONTROLLER_AXIS_LEFTX);
        sample.left_y  = axis(SDL_CONTROLLER_AXIS_LEFTY);
        sample.right_x = axis(SDL_CONTROLLER_AXIS_RIGHTX);
        sample.right_y = axis(SDL_CONTROLLER_AXIS_RIGHTY);
        sample.l2 = std::uint8_t(std::max(0, int(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT))) * 255 / 32767);
        sample.r2 = std::uint8_t(std::max(0, int(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT))) * 255 / 32767);
        const struct { SDL_GameControllerButton raw; std::uint32_t logical; } mapping[]{
            { SDL_CONTROLLER_BUTTON_A, Cross },
            { SDL_CONTROLLER_BUTTON_B, Circle },
            { SDL_CONTROLLER_BUTTON_X, Square },
            { SDL_CONTROLLER_BUTTON_Y, Triangle },
            { SDL_CONTROLLER_BUTTON_START, Options },
            { SDL_CONTROLLER_BUTTON_BACK, Share },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, L1 },
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, R1 },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, Left },
            { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Right },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, Up },
            { SDL_CONTROLLER_BUTTON_DPAD_DOWN, Down },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK, StickL },
            { SDL_CONTROLLER_BUTTON_RIGHTSTICK, StickR }
        };
        for (auto m : mapping) {
            if (SDL_GameControllerGetButton(controller, m.raw)) sample.buttons |= m.logical;
        }
        if (sample.l2 > 127) sample.buttons |= L2;
        if (sample.r2 > 127) sample.buttons |= R2;
    }
}
} // namespace

bool application_initialize(std::string& error) {
#if defined(__VITA__) || defined(__psp2__)
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    SceKernelFreeMemorySizeInfo meminfo{};
    meminfo.size = sizeof(meminfo);
    if (sceKernelGetFreeMemorySize(&meminfo) >= 0) {
        std::printf("[vita-mem] user_rw free=%d KB, cdram free=%d KB, phycont free=%d KB\n",
            meminfo.size_user / 1024, meminfo.size_cdram / 1024, meminfo.size_phycont / 1024);
    }
#endif
    SDL_SetMainReady();
#if defined(OR2_VITA_GL)
    const Uint32 flags = SDL_INIT_AUDIO | SDL_INIT_EVENTS;
#else
    const Uint32 flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS;
#endif
    if (SDL_Init(flags) < 0) {
        error = SDL_GetError();
        return false;
    }
    initialized = true;
    running = true;
    return true;
}

void application_shutdown() {
    outrun::vita_profile::shutdown_profiler();
    if (controller) SDL_GameControllerClose(controller);
    controller = nullptr;
    if (initialized) SDL_Quit();
#if defined(__VITA__) || defined(__psp2__)
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
#endif
    initialized = false;
    running = false;
}

bool application_running() { return running; }

void platform_sleep_ns(s64 ns) {
#if defined(__VITA__) || defined(__psp2__)
    if (ns > 1000) {
        sceKernelDelayThread(static_cast<SceUInt>(ns / 1000));
    }
#else
    if (ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    else std::this_thread::yield();
#endif
}

std::string application_home(int argc, char** argv) {
    std::string home = env("OR2_VITA_HOME") ? env("OR2_VITA_HOME") : "ux0:data/outrun2006";
    if (env("OR2_GAME_ROOT")) retail_root = env("OR2_GAME_ROOT");
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a.rfind("--home=", 0) == 0) home = a.substr(7);
        if (a.rfind("--data=", 0) == 0) retail_root = a.substr(7);
    }
    std::error_code ec;
    std::filesystem::create_directories(home + "/SaveGame", ec);
    if (retail_root.empty()) {
        std::ifstream file(home + "/retail-root.txt");
        std::getline(file, retail_root);
        if (!retail_root.empty() && retail_root.back() == '\r') retail_root.pop_back();
    }
    if (retail_root.empty()) {
        for (const char* path : {
            "ux0:data/outrun2006",
            "ux0:data/OutRun2006",
            "uma0:data/outrun2006",
            "uma0:data/OutRun2006",
            "app0:assets/game",
            "./OutRun2006"
        }) {
            if (outrun::platform::retail_asset_store_is_game_root(path)) {
                retail_root = path;
                break;
            }
        }
    }
    if (retail_root.empty()) {
        retail_root = home;
    }
    return home;
}

std::string application_retail_root() { return retail_root; }

void consoleInit(void*) { std::setvbuf(stdout, nullptr, _IOLBF, 0); }
void consoleUpdate(void*) { std::fflush(stdout); }
void consoleExit(void*) { std::fflush(stdout); }

void padConfigureInput(int, u64) {}
void padInitializeDefault(PadState* p) { *p = PadState{}; }

void padUpdate(PadState* p) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) running = false;
    }
    ++input_frame;
    read_pad(p->sample);
    const auto held = p->sample.connected ? p->sample.buttons : 0u;
    p->down = held & ~p->previous;
    p->up = p->previous & ~held;
    p->previous = held;
}

bool padIsConnected(const PadState* p) { return p->sample.connected; }
u64 padGetButtons(const PadState* p) { return p->sample.buttons; }
u64 padGetButtonsDown(const PadState* p) { return p->down; }
u64 padGetButtonsUp(const PadState* p) { return p->up; }

HidAnalogStickState padGetStickPos(const PadState* p, unsigned stick) {
    const auto x = stick ? p->sample.right_x : p->sample.left_x;
    const auto y = stick ? p->sample.right_y : p->sample.left_y;
    return { (int(x) - 128) * 256, -(int(y) - 128) * 256 };
}
