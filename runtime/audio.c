/* Host audio: SPU output (44.1 kHz stereo) to waveOut, and optionally to a WAV file.
 *
 * Environment:
 *   PSX_WAV=file.wav    also write everything the SPU produces (virtual-time exact)
 *   PSX_MUTE=1          no audio device (PSX_HEADLESS implies this)
 *   PSX_TRACE_AUDIO=1   report underruns/overflows at exit
 *
 * The emulator is paced to real time by display.c at the true virtual frame rate, so
 * production matches the device rate; a small ring absorbs jitter. If the ring
 * overfills (e.g. under PSX_FAST) the oldest audio is dropped to bound latency.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"

#define RATE        44100
#define NBUF        8
#define BUF_FRAMES  512
#define RING        16384u                 /* frames */
#define PREBUFFER   (3 * BUF_FRAMES)
#define MAX_QUEUED  (8 * BUF_FRAMES)

static HWAVEOUT wo;
static WAVEHDR hdr[NBUF];
static int16_t bufs[NBUF][BUF_FRAMES * 2];
static int16_t ring[RING][2];
static uint32_t head, tail;                /* free-running frame indices */
static int started;
static uint32_t underruns, overflows;

static FILE *wav;
static uint32_t wav_frames;

static void wav_header(void) {
    uint32_t data = wav_frames * 4, riff = 36 + data, fmt = 16, rate = RATE, bps = RATE * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
    fwrite(&fmt, 4, 1, wav); fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav);
    fwrite(&rate, 4, 1, wav); fwrite(&bps, 4, 1, wav); fwrite(&align, 2, 1, wav);
    fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav);
}

static void audio_report(void) {
    fprintf(stderr, "[audio] %u underruns, %u overflows, %u frames queued\n",
            underruns, overflows, head - tail);
}

static void wav_close(void) {
    if (!wav) return;
    wav_header();
    fclose(wav);
    wav = NULL;
}

void audio_init(void) {
    const char *path = getenv("PSX_WAV");
    if (path && (wav = fopen(path, "wb")) != NULL) {
        wav_header();
        atexit(wav_close);
    }
    if (getenv("PSX_HEADLESS") || getenv("PSX_MUTE")) return;
    WAVEFORMATEX fmt = { 0 };
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = RATE * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "warning: no audio device\n");
        wo = NULL;
        return;
    }
    if (getenv("PSX_TRACE_AUDIO")) atexit(audio_report);
    for (int i = 0; i < NBUF; i++) {
        hdr[i].lpData = (LPSTR)bufs[i];
        hdr[i].dwBufferLength = sizeof bufs[i];
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        hdr[i].dwFlags |= WHDR_DONE;       /* free for filling */
    }
}

void audio_pump(void) {
    if (!wo) return;
    if (!started && head - tail < PREBUFFER) return;
    int busy = 0;
    for (int i = 0; i < NBUF; i++) busy += !(hdr[i].dwFlags & WHDR_DONE);
    if (started && !busy) underruns++;     /* device ran dry since the last pump */
    started = 1;
    for (int i = 0; i < NBUF; i++) {
        if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
        if (head - tail < BUF_FRAMES) break;
        for (int k = 0; k < BUF_FRAMES; k++, tail++) {
            bufs[i][2 * k] = ring[tail % RING][0];
            bufs[i][2 * k + 1] = ring[tail % RING][1];
        }
        hdr[i].dwFlags &= ~(DWORD)WHDR_DONE;
        waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
    }
}

void audio_push(int16_t l, int16_t r) {
    if (wav) {
        int16_t f[2] = { l, r };
        fwrite(f, 2, 2, wav);
        wav_frames++;
    }
    if (!wo) return;
    if (head - tail >= MAX_QUEUED) {                 /* running ahead: drop */
        tail = head - MAX_QUEUED / 2;
        overflows++;
    }
    ring[head % RING][0] = l;
    ring[head % RING][1] = r;
    head++;
    if (!(head % BUF_FRAMES)) audio_pump();
}
