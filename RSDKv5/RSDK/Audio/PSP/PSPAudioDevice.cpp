#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspaudiolib.h>
#include <pspaudio.h>

uint8 AudioDevice::contextInitialized;
int32 AudioDevice::mixLock = -1;

// Scratch buffer used to receive the engine's float32 mix before it's converted
// down to the int16 PCM that the PSP's audio hardware actually takes.
static float mixScratch[PSP_NUM_AUDIO_SAMPLES * AUDIO_CHANNELS];

bool32 AudioDevice::Init()
{
    if (!contextInitialized) {
        contextInitialized = true;
        InitAudioChannels();
    }

    mixLock = sceKernelCreateSema("AudioMixLock", 0, 1, 1, NULL);
    if (mixLock < 0) {
        PrintLog(PRINT_NORMAL, "[PSP] Failed to create audio mix semaphore: %08x", mixLock);
        return false;
    }

    int32 result = pspAudioInit();
    if (result < 0) {
        PrintLog(PRINT_NORMAL, "[PSP] pspAudioInit failed: %08x", result);
        sceKernelDeleteSema(mixLock);
        mixLock = -1;
        return false;
    }

    pspAudioSetChannelCallback(0, AudioCallback, NULL);

    audioState = true;
    return true;
}

void AudioDevice::Release()
{
    if (mixLock < 0)
        return;

    pspAudioSetChannelCallback(0, NULL, NULL);

    LockAudioDevice();
    AudioDeviceBase::Release();
    UnlockAudioDevice();

    pspAudioEnd();

    sceKernelDeleteSema(mixLock);
    mixLock = -1;
}

void AudioDevice::InitAudioChannels() { AudioDeviceBase::InitAudioChannels(); }

int AudioDevice::StreamLoadThread(SceSize args, void *argp)
{
    ChannelInfo *channel = *(ChannelInfo **)argp;
    LockAudioDevice();
    LoadStream(channel);
    UnlockAudioDevice();
    sceKernelExitDeleteThread(0);
    return 0;
}

void AudioDevice::StartStreamLoadThread(ChannelInfo *channel)
{
    int32 thid = sceKernelCreateThread("StreamLoadThread", (SceKernelThreadEntry)StreamLoadThread, 0x12, 0x4000, 0, NULL);
    if (thid < 0) {
        LoadStream(channel);
        return;
    }

    sceKernelStartThread(thid, sizeof(channel), &channel);
}

void AudioDevice::AudioCallback(void *buf, unsigned int reqn, void *pdata)
{
    (void)pdata;

    if (reqn > PSP_NUM_AUDIO_SAMPLES)
        reqn = PSP_NUM_AUDIO_SAMPLES;

    LockAudioDevice();
    AudioDevice::ProcessAudioMixing(mixScratch, reqn * AUDIO_CHANNELS);
    UnlockAudioDevice();

    int16 *out = (int16 *)buf;
    for (uint32 i = 0; i < reqn * AUDIO_CHANNELS; ++i) {
        float sample = mixScratch[i] * 32767.0f;

        if (sample > 32767.0f)
            sample = 32767.0f;
        else if (sample < -32768.0f)
            sample = -32768.0f;

        out[i] = (int16)sample;
    }
}
