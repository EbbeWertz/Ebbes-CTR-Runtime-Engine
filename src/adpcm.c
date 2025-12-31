#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>

// #define MONO 1
// #define STEREO 2

#define SAMPLE_RATE 44100.0f //44100.0f
// #define SAMPLE_N_CHANNELS STEREO
// adpcm is fixed mono

// #define BYTES_PER_SAMPLE // isnt a variable integer. adpcm has 2 samples per byte (1 per 4bit nibble)
#define STREAM_BUF_SIZE (96*1024) // bytes per second = rate/2 = 22kB
#define N_BUFFERS 4 // triple buffering recommended for ADPCM

typedef enum {
    BUFF_REFILL_RES_OK,
    BUFF_REFILL_RES_FILEREAD_ERROR,
    BUFF_REFILL_RES_FILEREAD_EOF,
    BUFF_REFILL_RES_MEMORY_FLUSH_ERROR
} bufferRefillResult;

static ndspWaveBuf ndspBuffers[N_BUFFERS];
static u8 *adpcmBuffers[N_BUFFERS];
static ndspAdpcmData adpcmData = {0,0,0}; // single channel

// Hardcoded ADPCM coefficients from header
// sound.dsp
// static const u16 adpcmCoefA1[8] = {0xFFAA,0xFFAD,0x0001,0x018E,0x0035,0x008D,0x00BF,0x03AC};
// static const u16 adpcmCoefA2[8] = {0x0759,0x0678,0x07F8,0x04F0,0x0783,0x06BD,0x0174,0x03ED};
// out.dsp
static const u16 adpcmCoefA1[8] = {0x0066,0x0A65,0x0634,0x0DD5,0x028E,0x0B3E,0x085E,0x0F53};
static const u16 adpcmCoefA2[8] = {0x0700,0xFAFF,0x0077,0xF9E8,0x050F,0xFC3C,0xFF6F,0xF89C};


static bufferRefillResult fillBufferFromFile(const int buffer_index, FILE *file) {
    printf("filling buffer %d", buffer_index);
    // read new ADPCM data from file
    const size_t bytesRead = fread(adpcmBuffers[buffer_index], 1, STREAM_BUF_SIZE, file);
    if (bytesRead == 0) {
        if (feof(file)) return BUFF_REFILL_RES_FILEREAD_EOF;
        if (ferror(file)) return BUFF_REFILL_RES_FILEREAD_ERROR;
    }

    ndspBuffers[buffer_index].data_adpcm = adpcmBuffers[buffer_index];
    ndspBuffers[buffer_index].nsamples = bytesRead * 2; // each byte has 2 nibbles
    ndspBuffers[buffer_index].adpcm_data = &adpcmData;
    printf("%d, %d, %d", adpcmData.index, adpcmData.history0, adpcmData.history1);

    const Result flushResult = DSP_FlushDataCache(adpcmBuffers[buffer_index], bytesRead);
    if (flushResult) return BUFF_REFILL_RES_MEMORY_FLUSH_ERROR;

    ndspChnWaveBufAdd(0, &ndspBuffers[buffer_index]);
    printf(" - done\n");
    return BUFF_REFILL_RES_OK;
}

static void printRefillResult(const bufferRefillResult res) {
    switch (res) {
        case BUFF_REFILL_RES_FILEREAD_ERROR:
            printf("[ERROR] Could not read from file\n"); return;
        case BUFF_REFILL_RES_FILEREAD_EOF:
            printf("End of file reached\n"); return;
        case BUFF_REFILL_RES_MEMORY_FLUSH_ERROR:
            printf("[ERROR] Could not flush audio buffer\n");
        default: break;
    }
}

static bool initBuffers() {
    for (int i = 0; i < N_BUFFERS; i++) {
        adpcmBuffers[i] = linearAlloc(STREAM_BUF_SIZE);
        if (!adpcmBuffers[i]) return false;
        memset(&ndspBuffers[i], 0, sizeof(ndspWaveBuf));
        ndspBuffers[i].adpcm_data = &adpcmData;
    }
    return true;
}

static void initChannel() {
    ndspSetOutputMode(NDSP_OUTPUT_MONO);
    ndspChnReset(0);
    ndspChnInitParams(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_ADPCM);

    // set ADPCM coefficients
    u16 coefs[16];
    for (int i = 0; i < 8; i++) {
        coefs[i*2+0] = adpcmCoefA1[i];
        coefs[i*2+1] = adpcmCoefA2[i];
    }
    ndspChnSetAdpcmCoefs(0, coefs);
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    romfsInit();
    ndspInit();
    sleep(2); // ndsp needs some startup time (otherwise it stutters at start)

    FILE *adpcmFile = fopen("romfs:/audio/out.dsp", "rb");
    if (!adpcmFile) {
        printf("Failed to open ADPCM file\n");
        sleep(3);
        return 1;
    }

    // skip 96-byte header
    fseek(adpcmFile, 96, SEEK_SET);

    if (!initBuffers()) {
        printf("Failed to allocate ADPCM buffers\n");
        sleep(3);
        return 1;
    }

    initChannel();

    // pre-fill both buffers
    for (int i = 0; i < N_BUFFERS; i++) {
        const bufferRefillResult res = fillBufferFromFile(i, adpcmFile);
        printRefillResult(res);
    }

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;

        // check buffers for refill
        for (int i = 0; i < N_BUFFERS; i++) {
            if (ndspBuffers[i].status == NDSP_WBUF_DONE) {
                const bufferRefillResult res = fillBufferFromFile(i, adpcmFile);
                printRefillResult(res);
            }
        }

        gspWaitForVBlank();
        gfxSwapBuffers();
    }

    for (int i = 0; i < N_BUFFERS; i++)
        if (adpcmBuffers[i])
            linearFree(adpcmBuffers[i]);

    fclose(adpcmFile);
    ndspExit();
    romfsExit();
    gfxExit();
    return 0;
}
