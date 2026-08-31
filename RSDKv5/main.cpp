#include "RSDK/Core/RetroEngine.hpp"
#include "main.hpp"
#include <pspkernel.h>

#if __psp__
    PSP_MODULE_INFO("Sonic_Mania", 0, 1, 1);

    PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
    PSP_MAIN_THREAD_STACK_SIZE_KB(2048);

    // NOTE: this port calls PSP_HEAP_SIZE_KB(-128) inside main(), where the
    // macro expands to a declaration of an unused LOCAL and therefore does
    // nothing -- the game runs on newlib default heap. That looks like a
    // bug, but moving it to file scope so it takes effect is worse: -128
    // means "all memory except 128KB", which starves everything allocated
    // outside newlib (thread stacks, sceGuInit buffers, audio) and corrupts
    // tile layers on the title screen. Left as-is deliberately.

// HOME button support.
//
// The PSP only offers the quit prompt to an app that has registered an exit
// callback -- with none registered, HOME does nothing at all, which is why
// the only way out of this port was through the game's own menus.
//
// The callback deliberately does NOT call sceKernelExitGame(). The engine's
// shutdown path (end of RunRetroEngine) releases the audio device, writes
// Settings.ini via SaveSettingsINI() and closes storage; tearing the process
// down from the callback thread would skip all of that, and could truncate a
// save file that happened to be mid-write. Instead this just asks the main
// loop to stop, and main() exits for real once RunRetroEngine has unwound --
// at most one frame later, since the loop re-checks isRunning every pass.
static int PSP_ExitCallback(int arg1, int arg2, void *common)
{
    RSDK::RenderDevice::isRunning = false;
    return 0;
}

// Callbacks are only delivered to a thread parked in a callback-aware wait,
// hence sceKernelSleepThreadCB() rather than sceKernelSleepThread(). This is
// the standard pspsdk arrangement.
static int PSP_CallbackThread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", PSP_ExitCallback, NULL);
    if (cbid >= 0)
        sceKernelRegisterExitCallback(cbid);

    sceKernelSleepThreadCB();
    return 0;
}

static void PSP_SetupCallbacks()
{
    int thid = sceKernelCreateThread("update_thread", PSP_CallbackThread, 0x11, 0xFA0, THREAD_ATTR_USER, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
}
#endif

#if RETRO_STANDALONE
#define LinkGameLogic RSDK::LinkGameLogic
#else
#define EngineInfo RSDK::EngineInfo
#include <GameMain.h>
#define LinkGameLogic LinkGameLogicDLL
#endif

#if RETRO_PLATFORM == RETRO_WIN && !RETRO_RENDERDEVICE_SDL2

#if RETRO_RENDERDEVICE_DIRECTX9 || RETRO_RENDERDEVICE_DIRECTX11
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nShowCmd)
{
    RSDK::RenderDevice::hInstance     = hInstance;
    RSDK::RenderDevice::hPrevInstance = hPrevInstance;
    RSDK::RenderDevice::nShowCmd      = nShowCmd;

    return RSDK_main(1, &lpCmdLine, LinkGameLogic);
}
#else
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nShowCmd)
{
    return RSDK_main(1, &lpCmdLine, LinkGameLogic);
}
#endif

#elif RETRO_PLATFORM == RETRO_ANDROID
extern "C" {
void android_main(struct android_app *app);
}

void android_main(struct android_app *ap)
{
    app                                 = ap;
    app->onAppCmd                       = AndroidCommandCallback;
    app->activity->callbacks->onKeyDown = AndroidKeyDownCallback;
    app->activity->callbacks->onKeyUp   = AndroidKeyUpCallback;

    JNISetup *jni = GetJNISetup();
    // we make sure we do it here so init can chill safely before any callbacks occur
    Paddleboat_init(jni->env, jni->thiz);

    SwappyGL_init(jni->env, jni->thiz);
    SwappyGL_setAutoSwapInterval(false);
    SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);
    SwappyGL_setMaxAutoSwapIntervalNS(SWAPPY_SWAP_60FPS);

    getFD    = jni->env->GetMethodID(jni->clazz, "getFD", "([BB)I");
    writeLog = jni->env->GetMethodID(jni->clazz, "writeLog", "([BI)V");

    setLoading = jni->env->GetMethodID(jni->clazz, "setLoadingIcon", "([B)V");
    showLoading = jni->env->GetMethodID(jni->clazz, "showLoadingIcon", "()V");
    hideLoading = jni->env->GetMethodID(jni->clazz, "hideLoadingIcon", "()V");

    setPixSize = jni->env->GetMethodID(jni->clazz, "setPixSize", "(II)V");

#if RETRO_USE_MOD_LOADER
    fsExists      = jni->env->GetMethodID(jni->clazz, "fsExists", "([B)Z");
    fsIsDir       = jni->env->GetMethodID(jni->clazz, "fsIsDir", "([B)Z");
    fsDirIter     = jni->env->GetMethodID(jni->clazz, "fsDirIter", "([B)[Ljava/lang/String;");
    fsRecurseIter = jni->env->GetMethodID(jni->clazz, "fsRecurseIter", "([B)Ljava/lang/String;");
#endif

    GameActivity_setWindowFlags(app->activity,
                                AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_TURN_SCREEN_ON | AWINDOW_FLAG_LAYOUT_NO_LIMITS | AWINDOW_FLAG_FULLSCREEN
                                    | AWINDOW_FLAG_SHOW_WHEN_LOCKED,
                                0);

    RSDK_main(0, NULL, (void *)LinkGameLogic);

    Paddleboat_destroy(jni->env);
    SwappyGL_destroy();
}
#else
int32 main(int32 argc, char *argv[]) {
    #ifdef __psp__
    PSP_HEAP_SIZE_KB(-128);
    PSP_SetupCallbacks();

    int32 exitCode = RSDK_main(argc, argv, (void *)LinkGameLogic);

    // Hand the system back to the XMB. Reached both on a HOME quit (once the
    // engine's shutdown has run) and on a normal quit from the game's menu.
    sceKernelExitGame();
    return exitCode;
    #else
    return RSDK_main(argc, argv, (void *)LinkGameLogic);
    #endif
    }
#endif

int32 RSDK_main(int32 argc, char **argv, void *linkLogicPtr)
{
    RSDK::linkGameLogic = (RSDK::LogicLinkHandle)linkLogicPtr;

    RSDK::InitCoreAPI();

    int32 exitCode = RSDK::RunRetroEngine(argc, argv);

    RSDK::ReleaseCoreAPI();

    return exitCode;
}
