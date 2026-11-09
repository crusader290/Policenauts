/* Host audio output: 44.1 kHz stereo from the SPU to waveOut and/or a WAV file. */
#pragma once
#include <stdint.h>

void audio_init(void);
void audio_push(int16_t l, int16_t r);   /* one output frame */
void audio_pump(void);                   /* hand finished samples to the device */
