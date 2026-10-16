/* What SDL's own channel conversion does to the port's 8-channel PCM:
 * BB_AUDIO_DUMP gives post-gain PCM in the port's format; if the sink is a stereo
 * device, SDL converts 8ch -> 2ch with its own matrices before the device sees a
 * sample. This measures that conversion's level with the same libSDL3 the port links.
 *   ./sdl-mix-level out/audio-dump.port0.8ch.f32 [content_start_seconds]
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void stats(const char *label, const void *data, int len, const SDL_AudioSpec *spec, double skip_seconds)
{
    size_t frame = (size_t)SDL_AUDIO_BYTESIZE(spec->format) * spec->channels;
    if (!len || !frame) { printf("%-34s empty\n", label); return; }
    size_t frames = (size_t)len / frame;
    double peak = 0, sumsq = 0;
    size_t n = 0;
    for (size_t f = 0; f < frames; ++f) {
        if (f / (double)spec->freq < skip_seconds) continue;
        for (int c = 0; c < spec->channels; ++c) {
            double v = spec->format == SDL_AUDIO_F32
                           ? ((const float *)data)[f * spec->channels + c]
                           : ((const Sint16 *)data)[f * spec->channels + c] / 32768.0;
            if (fabs(v) > peak) peak = fabs(v);
            sumsq += v * v; ++n;
        }
    }
    double rms = n ? sqrt(sumsq / n) : 0;
    printf("%-34s frames=%zu peak=%.6f (%6.2f dBFS)  rms=%.6f (%6.2f dBFS)\n", label, frames, peak,
           20 * log10(peak ? peak : 1e-30), rms, 20 * log10(rms ? rms : 1e-30));
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <dump.8ch.f32> [content_start_s]\n", argv[0]); return 1; }
    double skip = argc > 2 ? atof(argv[2]) : 0.0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    Uint8 *raw = malloc((size_t)size);
    if (fread(raw, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "SDL audio init failed: %s\n", SDL_GetError()); return 1; }
    SDL_AudioSpec src = {SDL_AUDIO_F32, 8, 48000};
    stats("port PCM as dumped (8ch F32)", raw, (int)size, &src, skip);

    SDL_AudioSpec dst_f32 = {SDL_AUDIO_F32, 2, 48000};
    Uint8 *out = NULL; int out_len = 0;
    if (!SDL_ConvertAudioSamples(&src, raw, (int)size, &dst_f32, &out, &out_len)) {
        fprintf(stderr, "convert 8ch->2ch F32 failed: %s\n", SDL_GetError());
    } else {
        stats("after SDL 8ch->2ch F32", out, out_len, &dst_f32, skip);
        SDL_free(out); out = NULL;
        SDL_AudioSpec dst_s16 = {SDL_AUDIO_S16, 2, 48000};
        if (!SDL_ConvertAudioSamples(&dst_f32, out, out_len, &dst_s16, &out, &out_len)) {}
        if (SDL_ConvertAudioSamples(&src, raw, (int)size, &dst_s16, &out, &out_len))
            stats("after SDL 8ch->2ch S16 (device)", out, out_len, &dst_s16, skip);
        else fprintf(stderr, "convert to S16 failed: %s\n", SDL_GetError());
    }
    if (out) SDL_free(out);
    free(raw);
    return 0;
}
