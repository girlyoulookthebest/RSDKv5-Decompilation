#ifndef AUDIO_H
#define AUDIO_H

namespace RSDK
{

#define SFX_COUNT     (0)
#define CHANNEL_COUNT (0)

#define MIX_BUFFER_SIZE (0)
#define SAMPLE_FORMAT   float

#define AUDIO_FREQUENCY (0)
#define AUDIO_CHANNELS  (0)
struct ChannelInfo {
    float *samplePtr;
    float pan;
    float volume;
    int32 speed;
    size_t sampleLength;
    int32 bufferPos;
    int32 playIndex;
    uint32 loop;
    int16 soundID;
    uint8 priority;
    uint8 state;
};

namespace AudioDevice
{
    inline bool32 Init(){
        return true;
    }

    inline void Release(){
        return;
    }

    inline void ProcessAudioMixing(void *stream, int32 length){
        return;
    }

    inline void FrameInit(){
        return;
    }

    inline void HandleStreamLoad(ChannelInfo *channel, bool32 async){
        return;
    }

    static uint8 initializedAudioChannels = true;
    static uint8 audioState = true;
    static uint8 audioFocus = 0;
}

inline uint16 GetSfx(const char *sfxName)
{
    return -1;
}

inline int32 PlaySfx(uint16 sfx, uint32 loopPoint, uint32 priority)
{
    return -1;
}

inline void StopSfx(uint16 sfx)
{
    return;
}

#if RETRO_REV0U
inline void StopAllSfx()
{
    return;
}
#endif

inline int32 PlayStream(const char *filename, uint32 slot, uint32 startPos, uint32 loopPoint, bool32 loadASync)
{
        return -1;
}

inline void SetChannelAttributes(uint8 channel, float volume, float panning, float speed)
{
    return;
}

inline void StopChannel(uint32 channel)
{
    return;
}

inline void PauseChannel(uint32 channel)
{
    return;
}

inline void ResumeChannel(uint32 channel)
{
    return;
}

inline void PauseSound()
{
    return;
}

inline bool32 SfxPlaying(uint16 sfx)
{
    return false;
}

inline void ResumeSound()
{
    return;
}

inline bool32 IsSfxPlaying(uint32 channel)
{
        return false;
}

inline bool32 ChannelActive(uint32 channel)
{
        return false;
}

inline uint32 GetChannelPos(uint32 channel)
{
    return 0;
}
inline double GetVideoStreamPos()
{
    return -1.0;
}

inline void LoadSfxToSlot(char *filename, uint8 slot, uint8 plays, uint8 scope)
{
    return;
}
inline void LoadSfx(char *filePath, uint8 plays, uint8 scope)
{
    return;
}

inline void ClearStageSfx(){
    return;
}

#if RETRO_REV0U
namespace Legacy
{
#define LEGACY_TRACK_COUNT (0x10)
struct TrackInfo {
    char fileName[0x40];
    bool32 trackLoop;
    uint32 loopPoint;
};

 static int32 globalSFXCount = 0;
 static int32 stageSFXCount = 0;

 static int32 musicVolume = 0;
 static int32 sfxVolume = 0;
 static int32 bgmVolume = 0;
 static int32 musicCurrentTrack = 0;
 static int32 musicChannel = 0;

extern TrackInfo musicTracks[LEGACY_TRACK_COUNT];

inline void SetMusicTrack(const char *filePath, uint8 trackID, bool32 loop, uint32 loopPoint)
{
    return;
}

inline int32 PlayMusic(int32 trackID)
{
    return -1;
}

inline void SetMusicVolume(int32 volume)
{
    return;
}

inline void LoadSfx(char *filename, uint8 slot, uint8 scope)
{
    return;
}

inline void StopMusic() { StopChannel(musicChannel); }
inline int32 PlaySfx(int32 sfxID, bool32 loop) { return RSDK::PlaySfx(sfxID, loop, 0xFF); }
inline void StopSfx(int32 sfxID) { RSDK::StopSfx(sfxID); }

namespace v3
{
extern char globalSfxNames[SFX_COUNT][0x40];
extern char stageSfxNames[SFX_COUNT][0x40];
inline void SetSfxAttributes(int32 channelID, int32 loop, int8 pan)
{
    return;
}
#if RETRO_USE_MOD_LOADER
inline void SetSfxName(const char *sfxName, int32 sfxID, bool32 global)
{
    return;
}
#endif

} // namespace v3

namespace v4
{
extern float musicRatio;
extern char sfxNames[SFX_COUNT][0x40];
inline void SetSfxName(const char *sfxName, int32 sfxID)
{
    return;
}
inline void SetSfxAttributes(int32 sfxID, int32 loop, int8 pan)
{
    return;
}
inline void SwapMusicTrack(const char *filePath, uint8 trackID, uint32 loopPoint, uint32 ratio)
{
    return;
}
} // namespace v4

} // namespace Legacy
#endif

} // namespace RSDK

#endif