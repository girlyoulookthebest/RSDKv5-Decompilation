#include <pspthreadman.h>

#define LockAudioDevice()   sceKernelWaitSema(AudioDevice::mixLock, 1, NULL)
#define UnlockAudioDevice() sceKernelSignalSema(AudioDevice::mixLock, 1)

namespace RSDK
{
class AudioDevice : public AudioDeviceBase
{
public:
    static int32 mixLock;

    static bool32 Init();
    static void Release();

    static void FrameInit() {}

    inline static void HandleStreamLoad(ChannelInfo *channel, bool32 async)
    {
        if (async)
            StartStreamLoadThread(channel);
        else
            LoadStream(channel);
    }

private:
    static uint8 contextInitialized;

    static void InitAudioChannels();

    static void StartStreamLoadThread(ChannelInfo *channel);
    static int StreamLoadThread(SceSize args, void *argp);

    static void AudioCallback(void *buf, unsigned int reqn, void *pdata);
};
} // namespace RSDK
