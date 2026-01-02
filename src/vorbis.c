#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <tremor/ivorbisfile.h>
#include <tremor/ivorbiscodec.h>

#define MUSIC1_PATH "romfs:/audio/song1_pcm16_44100hz_stereo.ogg"

#define MUSIC2_PATH "romfs:/audio/song2_pcm16_48000hz_mono.ogg"

#define STREAM_BUF_SIZE (16*1024) // about 170ms for stereo 16bit PCM at 48kHz
#define N_BUFFERS_PER_CHANNEL 3 // 2 (dual buffering, good for sfx) or 3 (triple, more robust for music)
#define AUDIO_THREAD_STACK_SIZE (1024*1024) // 1MB

typedef struct {
    const char *filePath;
    bool pending;
} AudioRequest;

volatile AudioRequest currentAudioRequest = {nullptr, false};

typedef enum {
    BUFF_REFILL_RES_OK,
    BUFF_REFILL_RES_FILEREAD_ERROR,
    BUFF_REFILL_RES_FILEREAD_EOF,
    BUFF_REFILL_RES_MEMORY_FLUSH_ERROR
} bufferRefillResult;

LightEvent audioRequestEvent;
volatile bool audioThreadRunning = true;

ndspWaveBuf ndspBuffers[N_BUFFERS_PER_CHANNEL];
s16 *pcmBuffers[N_BUFFERS_PER_CHANNEL];

void play(const char* filePath) {
    currentAudioRequest.filePath = filePath;
    currentAudioRequest.pending = true;
    LightEvent_Signal(&audioRequestEvent);
}

bufferRefillResult fillBufferFromFile(const int buffer_index, OggVorbis_File* vorbisFile, const u8* bytesPerSample) {
    // read and decode new PCM data from file
    // ov_read does not always read the specified bytes if the current stream blocks are not a multiple of the buffer size.
    // therefore multiple reads in a loop
    long totalBytesRead = 0;
    while (totalBytesRead < STREAM_BUF_SIZE) {
        char* pcmBufferWithOffset = ((char*)pcmBuffers[buffer_index]) + totalBytesRead;
        const int bytesToRead = STREAM_BUF_SIZE - totalBytesRead;
        const long bytesRead = ov_read(vorbisFile, pcmBufferWithOffset, bytesToRead, nullptr);
        if (bytesRead == 0) return BUFF_REFILL_RES_FILEREAD_EOF;
        if (bytesRead < 0) return BUFF_REFILL_RES_FILEREAD_ERROR;
        totalBytesRead += bytesRead;
    }

    // update buffer struct
    ndspBuffers[buffer_index].data_pcm16 = pcmBuffers[buffer_index];
    ndspBuffers[buffer_index].nsamples = totalBytesRead / *bytesPerSample;
    // ensure buffer is actually in memory and not in CPU cache
    const Result flushResult = DSP_FlushDataCache(pcmBuffers[buffer_index], totalBytesRead);
    if (flushResult) return BUFF_REFILL_RES_MEMORY_FLUSH_ERROR;
    // queue buffer
    ndspChnWaveBufAdd(0, &ndspBuffers[buffer_index]);
    return BUFF_REFILL_RES_OK;
}

inline void printRefillResult(const bufferRefillResult res) {
    switch (res) {
        case BUFF_REFILL_RES_FILEREAD_ERROR:
            printf("[ERROR] Could not read from file\n");
            return;
        case BUFF_REFILL_RES_FILEREAD_EOF:
            printf("End of file reached\n");
            return;
        case BUFF_REFILL_RES_MEMORY_FLUSH_ERROR:
            printf("[ERROR] Could not flush audio buffer\n");
        default: break;
    }
}

bool initBuffers() {
    for (int i = 0; i < N_BUFFERS_PER_CHANNEL; i++) {
        // allocate linear memory for pcm data buffers
        pcmBuffers[i] = linearAlloc(STREAM_BUF_SIZE);
        if (!pcmBuffers[i]) return false;
        // initialise ndsp buffer structs
        memset(&ndspBuffers[i], 0, sizeof(ndspWaveBuf));
    }
    return true;
}

void setupChannel(const vorbis_info* vi) {
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnInitParams(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, (float)vi->rate);
    ndspChnSetFormat(0, vi->channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
}

void audioCallback([[maybe_unused]] void* unused_) {
    LightEvent_Signal(&audioRequestEvent);
}


inline void handleThreadRequest(OggVorbis_File* vorbisFile, u8* bytesPerSample) {

    if (currentAudioRequest.pending) {
        printf("play requested\n");
        currentAudioRequest.pending = false;
        // open file
        FILE* file = fopen(currentAudioRequest.filePath, "rb");
        const int error = ov_open(file, vorbisFile, nullptr, 0);
        if (error) {
            printf("Failed to open ogg vorbis file: %s\n", currentAudioRequest.filePath);
            fclose(file);
            sleep(3);
            return;
        }
        const vorbis_info* vi = ov_info(vorbisFile, -1);
        *bytesPerSample = vi->channels * 2;
        // setup channel
        setupChannel(vi);
        // prefill buffers
        for (int i = 0; i < N_BUFFERS_PER_CHANNEL; i++) {
            printf("buff %d prefill\n", i);
            const bufferRefillResult res = fillBufferFromFile(i, vorbisFile, bytesPerSample);
            printRefillResult(res);
            if (res == BUFF_REFILL_RES_FILEREAD_EOF)
                ov_clear(vorbisFile);
        }
    }

    for (int i = 0; i < N_BUFFERS_PER_CHANNEL; i++) {
        if (ndspBuffers[i].status == NDSP_WBUF_DONE) {
            printf("buff %d refill\n", i);
            const bufferRefillResult res = fillBufferFromFile(i, vorbisFile, bytesPerSample);
            printRefillResult(res);
        }
    }
}

void audioThread([[maybe_unused]] void* unused_) {
    OggVorbis_File currentVorbisFile;
    u8 currentBytesPerSample;
    printf("Audio thread ready\n");

    // main audio thread loop
    while (audioThreadRunning) {
        handleThreadRequest(&currentVorbisFile, &currentBytesPerSample);
        LightEvent_Wait(&audioRequestEvent);
    }
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    romfsInit();
    ndspInit();
    LightEvent_Init(&audioRequestEvent, RESET_ONESHOT);
    sleep(2); // ndsp needs some startup time (otherwise it stutters at start)

    if (!initBuffers()) {
        printf("Failed to allocate linear PCM buffers\n");
        sleep(3);
        return 1;
    }

    ndspSetCallback(audioCallback, nullptr);

    int32_t priority = 0x30; // main thread priority
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority -= 1; // increase priority (aka smaller priority value)
    priority = priority < 0x18 ? 0x18 : priority;
    priority = priority > 0x3F ? 0x3F : priority;
    const Thread audioThreadId = threadCreate(audioThread, nullptr,AUDIO_THREAD_STACK_SIZE, priority,-1, false);

    // main loop
    while (aptMainLoop()) {
        hidScanInput();
        const u32 keysdown = hidKeysDown();
        if (keysdown & KEY_START) break;
        if (keysdown & KEY_A) {
            printf("requesting play song 1\n");
            play(MUSIC1_PATH);
        } else if (keysdown & KEY_B) {
            printf("requesting play song 2\n");
            play(MUSIC2_PATH);
        }
        gspWaitForVBlank();
        gfxSwapBuffers();
    }

    audioThreadRunning = false;
    LightEvent_Signal(&audioRequestEvent);
    threadJoin(audioThreadId, UINT64_MAX);
    threadFree(audioThreadId);

    for (int i = 0; i < N_BUFFERS_PER_CHANNEL; i++)
        if (pcmBuffers[i])
            linearFree(pcmBuffers[i]);

    ndspExit();
    romfsExit();
    gfxExit();
    return 0;
}
