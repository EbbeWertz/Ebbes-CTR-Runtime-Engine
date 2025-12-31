#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>

#define MONO 1
#define STEREO 2

// sample properties
#define SAMPLE_RATE 44100.0f //44100.0f
#define SAMPLE_N_CHANNELS STEREO

#define BYTES_PER_SAMPLE (2 * SAMPLE_N_CHANNELS) // 2 bytes for 16bit
#define STREAM_BUF_SIZE (16*1024)
#define N_BUFFERS_PER_CHANNEL 3 // 2 (dual buffering, good for sfx) or 3 (triple, more robust for music)

#define AUDIO_THREAD_STACK_SIZE (1024*1024) // 1MB

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

bufferRefillResult fillBufferFromFile(const int buffer_index, FILE *file) {
    printf("filling buffer %d\n", buffer_index);
    // read new PCM data from file
    const size_t bytesRead = fread(pcmBuffers[buffer_index], 1,STREAM_BUF_SIZE, file);
    if (bytesRead == 0) {
        if (feof(file)) return BUFF_REFILL_RES_FILEREAD_EOF;
        if (ferror(file)) return BUFF_REFILL_RES_FILEREAD_ERROR;
    }
    // update buffer struct
    ndspBuffers[buffer_index].data_pcm16 = pcmBuffers[buffer_index];
    ndspBuffers[buffer_index].nsamples = bytesRead / BYTES_PER_SAMPLE;
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

void initChannel() {
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnInitParams(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, SAMPLE_RATE);
    ndspChnSetFormat(0, SAMPLE_N_CHANNELS == STEREO ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
}

void audioCallback([[maybe_unused]] void* unused_) {
    LightEvent_Signal(&audioRequestEvent);
}


inline void handleThreadRequest(FILE* pcmDataFileHandle) {
    for (int i = 0; i < N_BUFFERS_PER_CHANNEL; i++) {
        if (ndspBuffers[i].status == NDSP_WBUF_DONE) {
            const bufferRefillResult res = fillBufferFromFile(i, pcmDataFileHandle);
            printRefillResult(res);
        }
    }
}

void audioThread([[maybe_unused]] void* unused_) {
    printf("audio thread started\n");

    // open file handle
    FILE *pcmDataFileHandle = fopen("romfs:/audio/song1_pcm16_44100hz_stereo.raw", "rb");
    if (!pcmDataFileHandle) {
        printf("Failed to open PCM file\n");
        sleep(3);
        return;
    }

    // prefill buffers
    for (int i = 0; i < N_BUFFERS_PER_CHANNEL; i++) {
        const bufferRefillResult res = fillBufferFromFile(i, pcmDataFileHandle);
        printRefillResult(res);
    }

    // main audio thread loop
    while (audioThreadRunning) {
        handleThreadRequest(pcmDataFileHandle);
        LightEvent_Wait(&audioRequestEvent);
    }
    fclose(pcmDataFileHandle);
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

    initChannel();
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
        if (hidKeysDown() & KEY_START) break;
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
