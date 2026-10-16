/* Independent probe of the SDL3 device path the port uses (src/runtime_audio.c):
 * list playback devices, open the default one with the port's exact spec
 * (8ch F32 48000), resume it and show whether the device actually drains the queue.
 * Prints only; plays a 0.6 s quiet tone only if --tone is passed. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int want_tone = argc > 1 && !SDL_strcmp(argv[1], "--tone");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        printf("PROBE: SDL_InitSubSystem(AUDIO) FAILED: %s\n", SDL_GetError());
        return 1;
    }
    printf("PROBE: SDL %d.%d.%d, driver=%s\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_MICRO_VERSION, SDL_GetCurrentAudioDriver());
    int n = 0;
    SDL_AudioDeviceID *devs = SDL_GetAudioPlaybackDevices(&n);
    printf("PROBE: %d playback device(s)\n", n);
    for (int i = 0; i < n; ++i) {
        SDL_AudioSpec spec;
        bool is_default = (devs[i] == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        printf("PROBE:   [%d] id=%u name='%s'%s\n", i, (unsigned)devs[i],
               SDL_GetAudioDeviceName(devs[i]), is_default ? " (DEFAULT_PLAYBACK)" : "");
        if (SDL_GetAudioDeviceFormat(devs[i], &spec, NULL)) {
            printf("PROBE:       device format: %d Hz, %d ch, fmt=0x%x, paused=%d\n",
                   spec.freq, spec.channels, (unsigned)spec.format,
                   (int)SDL_AudioDevicePaused(devs[i]));
        } else {
            printf("PROBE:       SDL_GetAudioDeviceFormat failed: %s\n", SDL_GetError());
        }
    }
    printf("PROBE: default playback device id=%u name='%s'\n",
           (unsigned)SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
           SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK));

    /* Exactly what runtime_audio.c does. */
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 8, 48000};
    SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!st) {
        printf("PROBE: SDL_OpenAudioDeviceStream(8ch F32 48000) FAILED: %s\n", SDL_GetError());
        SDL_free(devs);
        return 2;
    }
    printf("PROBE: OpenAudioDeviceStream ok\n");
    SDL_AudioDeviceID got = SDL_GetAudioStreamDevice(st);
    printf("PROBE: stream landed on id=%u name='%s' paused=%d\n", (unsigned)got,
           SDL_GetAudioDeviceName(got), (int)SDL_AudioDevicePaused(got));
    SDL_AudioSpec dst;
    if (SDL_GetAudioStreamFormat(st, NULL, &dst))
        printf("PROBE: stream output format: %d Hz, %d ch, fmt=0x%x\n", dst.freq, dst.channels, (unsigned)dst.format);
    SDL_ResumeAudioStreamDevice(st);
    printf("PROBE: after resume paused=%d\n", (int)SDL_AudioDevicePaused(got));

    /* Push 0.5 s of 8ch F32 and watch the queue drain: a live device pulls. */
    const int frames = 24000, chans = 8;
    float *buf = calloc((size_t)frames * chans, sizeof(float));
    printf("PROBE: tone=%d (silence still exercises the queue)\n", want_tone);
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < chans; ++c)
            buf[f * chans + c] = want_tone ? (float)(0.1 * sin(2 * M_PI * 440.0 * f / 48000.0)) : 0.0f;
    size_t pushed = (size_t)frames * chans * sizeof(float);
    SDL_PutAudioStreamData(st, buf, (int)pushed);
    printf("PROBE: pushed %zu bytes, queued=%d\n", pushed, SDL_GetAudioStreamQueued(st));
    for (int i = 0; i < 8; ++i) {
        SDL_Delay(250);
        printf("PROBE: +%dms queued=%d\n", (i + 1) * 250, SDL_GetAudioStreamQueued(st));
    }
    SDL_DestroyAudioStream(st);
    free(buf);
    SDL_free(devs);
    return 0;
}
