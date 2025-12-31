#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>

/*
 *  ==========================================
 *  BUFFER LAYOUT
 *  ==========================================
 *  | Group name | Channel | Size[kB] | Purpose (note: ADPCM cannot be streamed)
 *  |------------|---------|----------|---------------------------------------------------
 *  | MUSIC      | 22-23   | 2x  48   | Triple buffered music streaming
 *  | ADPCM_XL   | 20-21   | 2x  128  | Large single buffer ADPCM
 *  | GP_L       | 16-19   | 4x  32   | General purpose large
 *  | GP_S       | 00-15   | 16x 16   | General purpose small
 *
 *  Use cases:
 *  MUSIC
 *   - Streaming music from an ogg vorbis decoder
 *  ADPCM_XL
 *   - Playing extra long ADPCM encoded sounds (eg. ambient sounds)
 *  GP_L
 *   - Stereo PCM16 streaming (dual 16 kB buffer streamed)
 *   - Medium length ADPCM sounds (single buffer oneshot)
 *  GP_S
 *   - Mono PCM16 or any PCM8 streaming (dual 8 kB buffer streamed)
 *   - Short length ADPCM sounds (single buffer oneshot)
 *   - Fallback for stereo PCM16 if GP_L is full
 *
 *  ==========================================
 *  16bit MONO PCM/ADPCM REFERENCE
 *  ==========================================
 *  | Buffer    | Playback Time [ms]/[ms] PCM/ADPCM per sample rate
 *  | Size [kB] | 48'000 Hz  | 44'100 Hz  | 22'050 Hz   | 11'025 Hz   | 8'000 Hz
 *  |-----------|------------|------------|-------------|-------------|----------
 *  | 8 kB      |  85 /  341 |  92 /  372 |  186 /  743 |  372 / 1.5s |  512 / 2.0s
 *  | 16 kB     | 170 /  683 | 186 /  743 |  372 / 1.5s |  743 / 3.0s | 1.0s / 4.1s
 *  | 32 kB     | 341 / 1.4s | 372 / 1.5s |  743 / 3.0s | 1.5s / 5.9s | 2.0s / 8.2s
 *  | 64 kB     | 683 / 2.7s | 743 / 3.0s | 1.5s / 5.9s | 3.0s / 12s  | 4.1s / 16s
 *
 *  - 16bit stereo PCM will be the PCM time / 2 (stereo adpcm isnt supported by ndsp)
 *  - 8bit mono PCM will be the PCM time * 2
 *  - 8bit stereo PCM will be equal to the pcm time
 */



#define MUSIC1_PATH "romfs:/audio/song1_pcm16_44100hz_stereo.raw"
#define MUSIC1_SAMPLE_RATE 44100.0f
#define MUSIC1_FORMAT NDSP_FORMAT_STEREO_PCM16

#define MUSIC2_PATH "romfs:/audio/song2_pcm16_48000hz_mono.raw"
#define MUSIC2_SAMPLE_RATE 48000.0f
#define MUSIC2_FORMAT NDSP_FORMAT_MONO_PCM16

#define STREAM_BUF_SIZE (32*1024) // about 170ms for stereo 16bit PCM at 48kHz
#define N_BUFFERS_PER_CHANNEL 3 // 2 (dual buffering, good for sfx) or 3 (triple, more robust for music)
#define AUDIO_THREAD_STACK_SIZE (1024*1024) // 1MB

typedef struct {
    const char *filePath;
    u16 format;
    float sampleRate;
    bool pending;
} AudioRequest;

volatile AudioRequest currentAudioRequest = {nullptr, 0, 0, 0};

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

void play(const char* filePath, const u16 format, const float sampleRate) {
    currentAudioRequest.filePath = filePath;
    currentAudioRequest.format = format;
    currentAudioRequest.sampleRate = sampleRate;
    currentAudioRequest.pending = true;
    LightEvent_Signal(&audioRequestEvent);
}

inline int getBytesPerSample(const u16 format) {
    const int nChannels = format & 0x03; // 0x01 = mono, 0x02 = stereo (mask=0x03)
    const int bytesPerChannel = format & 0x04 ? 2 : 1; // 0x00 = 8bit, 0x04 = 16bit (mask=0x04)
    return nChannels * bytesPerChannel;
}

bufferRefillResult fillBufferFromFile(const int buffer_index, FILE *file) {
    // read new PCM data from file
    const size_t bytesRead = fread(pcmBuffers[buffer_index], 1,STREAM_BUF_SIZE, file);
    if (bytesRead == 0) {
        if (feof(file)) return BUFF_REFILL_RES_FILEREAD_EOF;
        if (ferror(file)) return BUFF_REFILL_RES_FILEREAD_ERROR;
    }
    // update buffer struct
    ndspBuffers[buffer_index].data_pcm16 = pcmBuffers[buffer_index];
    ndspBuffers[buffer_index].nsamples = bytesRead / getBytesPerSample(currentAudioRequest.format);
    // ensure buffer is actually in memory and not in CPU cache
    const Result flushResult = DSP_FlushDataCache(pcmBuffers[buffer_index], bytesRead);
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

void setupChannel(const u16 format, const float sampleRate) {
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnInitParams(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, sampleRate);
    ndspChnSetFormat(0, format);
}

void audioCallback([[maybe_unused]] void* unused_) {
    LightEvent_Signal(&audioRequestEvent);
}


inline void handleThreadRequest(FILE** pcmFile) {

    if (currentAudioRequest.pending) {
        printf("play requested\n");
        currentAudioRequest.pending = false;
        // setup channel
        setupChannel(currentAudioRequest.format, currentAudioRequest.sampleRate);
        // open file
        *pcmFile = fopen(currentAudioRequest.filePath, "rb");
        if (!*pcmFile) {
            printf("Failed to open PCM file: %s\n", currentAudioRequest.filePath);
            sleep(3);
            return;
        }
        // prefill buffers
        for (int i = 0; i < N_BUFFERS_PER_CHANNEL; i++) {
            printf("buff %d prefill\n", i);
            const bufferRefillResult res = fillBufferFromFile(i, *pcmFile);
            printRefillResult(res);
            if (res == BUFF_REFILL_RES_FILEREAD_EOF)
                fclose(*pcmFile);
        }
    }

    for (int i = 0; i < N_BUFFERS_PER_CHANNEL; i++) {
        if (ndspBuffers[i].status == NDSP_WBUF_DONE) {
            printf("buff %d refill\n", i);
            const bufferRefillResult res = fillBufferFromFile(i, *pcmFile);
            printRefillResult(res);
        }
    }
}

void audioThread([[maybe_unused]] void* unused_) {
    FILE *currentPcmFile = nullptr;
    printf("Audio thread ready\n");

    // main audio thread loop
    while (audioThreadRunning) {
        handleThreadRequest(&currentPcmFile);
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
        u32 keysdown = hidKeysDown();
        if (keysdown & KEY_START) break;
        if (keysdown & KEY_A) {
            printf("requesting play song 1\n");
            play(MUSIC1_PATH, MUSIC1_FORMAT, MUSIC1_SAMPLE_RATE);
        } else if (keysdown & KEY_B) {
            printf("requesting play song 2\n");
            play(MUSIC2_PATH, MUSIC2_FORMAT, MUSIC2_SAMPLE_RATE);
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
