/* libSceAudioOut on SDL3 audio streams. sceAudioOutOutput returns once per
 * buffer period on a steady clock, like PS4 hardware: FMOD's output thread
 * reads 256-frame slots out of its mixer's 512-frame ring, and returning in
 * bursts (whenever the device pulls a quantum) lets it overtake the mixer and
 * play stale halves - a click every 10.7 ms. The lowest SDL queue level of
 * each window is steered to two buffers (silence prefill on underrun, ±3%
 * cadence corrections), which also absorbs device clock drift. Without an audio device (headless runs, BB_AUDIO=none)
 * ports only follow the clock. BB_AUDIO_DEVICE=1 opens the device even in a warm run (sound
 * diagnostics that must stay hidden); BB_AUDIO_STATS=1 prints queue stats, BB_AUDIO_DUMP=<path>
 * dumps each port's post-gain PCM. */
#define _GNU_SOURCE
#include "runtime.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <SDL3/SDL.h>

#define PORTS 25
#define ERR_NOT_OPENED ((int32_t)0x80260001)
#define ERR_INVALID_PORT ((int32_t)0x80260003)
#define ERR_INVALID_POINTER ((int32_t)0x80260004)
#define ERR_PORT_FULL ((int32_t)0x80260005)
#define ERR_INVALID_SIZE ((int32_t)0x80260006)
#define ERR_INVALID_FORMAT ((int32_t)0x80260007)
#define ERR_INVALID_FREQ ((int32_t)0x80260008)
#define ERR_INVALID_VOLUME ((int32_t)0x80260009)
#define ERR_INVALID_TYPE ((int32_t)0x8026000A)
#define ERR_ALREADY_INIT ((int32_t)0x8026000E)
#define ERR_NOT_INIT ((int32_t)0x8026000F)
#define VOLUME_0DB 32768

typedef struct {
    int used, type, channels, is_float, frames, sample_bytes, std_layout;
    int32_t volume[8];
    int32_t vol_min, vol_max;       /* range of the volumes the game set */
    unsigned vol_calls;
    SDL_AudioStream *stream;
    uint64_t next_deadline_ns; /* next return of sceAudioOutOutput */
    int64_t adjust_ns; int window_min, window_count; /* queue level control */
    uint64_t last_output_us;
    pthread_mutex_t lock;
    FILE *dump;                     /* BB_AUDIO_DUMP: raw converted PCM per port */
    uint64_t push_bytes, silence_bytes; /* bytes queued to the device, of which silence */
    unsigned put_failed;            /* SDL_PutAudioStreamData failures: a dead stream is silent */
    float pm_peak; double pm_sumsq; unsigned long long pm_samples; /* BB_AUDIO_STATS: final device buffer */
    int stats;
    uint64_t stat_start_ns, stat_last_ns, stat_max_gap_ns; /* BB_AUDIO_STATS */
    unsigned stat_starved, stat_buffers; int stat_min_queued;
} Port;
typedef struct { uint16_t output; uint8_t channel, reserved; int16_t volume; uint16_t reroute; uint64_t flag, reserved64[2]; } PortState;
_Static_assert(sizeof(PortState)==32,"AudioOut port state layout");

static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static Port ports[PORTS];
static int initialized, sdl_ready=-1;
static size_t buffers_out, ports_opened;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void sleep_until(uint64_t deadline) {
    struct timespec t={(time_t)(deadline/1000000000u),(long)(deadline%1000000000u)};
    bb_sleep_until(&t);
}
static int sdl_audio(void) {
    if (sdl_ready<0) {
        const char *mode=getenv("BB_AUDIO");
        sdl_ready = (!mode || strcmp(mode,"none")) && SDL_InitSubSystem(SDL_INIT_AUDIO);
        printf("Runtime: audio backend %s\n", sdl_ready ? SDL_GetCurrentAudioDriver() : "timer (silent)");
    }
    return sdl_ready;
}
/* Which sink a port feeds. A warm run (BB_WARM_ONLY) keeps the timer sink: an invisible compile
 * pass must not play the game. BB_AUDIO_DEVICE overrides that: "1" opens the real device even in
 * a warm run (so the sound path can be exercised without a visible launch), "none"/"0" forces
 * the timer sink. */
static int audio_want_device(void) {
    const char *override_=getenv("BB_AUDIO_DEVICE");
    if (override_ && override_[0]) return strcmp(override_,"none") && strcmp(override_,"0");
    const char *warm=getenv("BB_WARM_ONLY");
    return !(warm && warm[0]=='1');
}
/* A paused stream never plays: SDL_OpenAudioDeviceStream hands the device back paused, so the
 * resume below matters. Naming the sink catches a stream that landed on the wrong device. */
static void SDLCALL postmix(void *userdata, const SDL_AudioSpec *spec, float *buffer, int buflen) {
    Port *p=userdata;
    (void)spec;
    if (!p || !buffer) return;
    for (int i=0;i<buflen;++i) {
        float v=buffer[i], a=v<0 ? -v : v;
        if (a>p->pm_peak) p->pm_peak=a;
        p->pm_sumsq+=(double)v*v;
    }
    p->pm_samples+=(unsigned long long)(buflen>0 ? buflen : 0);
}
static void report_audio_device(Port *p, int id) {
    SDL_AudioStream *stream=p->stream;
    SDL_AudioDeviceID dev=SDL_GetAudioStreamDevice(stream);
    const char *name=SDL_GetAudioDeviceName(dev);
    SDL_AudioSpec got, native;
    int have_got=SDL_GetAudioStreamFormat(stream,NULL,&got);
    printf("Runtime: audio port %d -> device '%s' (id %u), paused=%s\n",id,name ? name : "?",(unsigned)dev,
           SDL_AudioDevicePaused(dev) ? "yes (silent until resumed)" : "no");
    if (have_got) printf("Runtime: audio port %d: stream %d Hz, %d ch, format %s\n",
                         id,got.freq,got.channels,got.format==SDL_AUDIO_F32 ? "F32" : "other");
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&native,NULL))
        printf("Runtime: audio port %d: default device natively %d Hz, %d ch, %s\n",
               id,native.freq,native.channels,native.format==SDL_AUDIO_F32 ? "F32" : "other");
    /* The final buffer in the device's format, after every conversion, immediately before the
     * device is fed: the sink's level, measurable from inside the game (BB_AUDIO_STATS). */
    if (!SDL_SetAudioPostmixCallback(dev,postmix,p))
        fprintf(stderr,"Runtime: audio port %d: no postmix tap (%s)\n",id,SDL_GetError());
}
static int port_range(int type, int *first, int *last) {
    switch (type) {
    case 0: *first=0; *last=7; return 1;      /* main */
    case 1: *first=8; *last=8; return 1;      /* bgm */
    case 2: *first=9; *last=12; return 1;     /* voice */
    case 3: *first=13; *last=16; return 1;    /* personal */
    case 4: *first=17; *last=20; return 1;    /* pad speaker */
    case 126: *first=21; *last=22; return 1;  /* audio3d */
    case 127: *first=23; *last=24; return 1;  /* aux */
    default: return 0;
    }
}
static Port *port_of(int32_t handle, int32_t *error) {
    int id=handle & 0xff;
    if ((handle & 0x3f000000)!=0x20000000 || id>=PORTS) { *error=ERR_INVALID_PORT; return NULL; }
    if (!ports[id].used) { *error=ERR_NOT_OPENED; return NULL; }
    return &ports[id];
}

static ABI int32_t audio_init(void) {
    pthread_mutex_lock(&table_lock);
    int32_t r=initialized ? ERR_ALREADY_INIT : 0;
    initialized=1;
    pthread_mutex_unlock(&table_lock);
    return r;
}
static ABI int32_t audio_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param) {
    (void)user; (void)index;
    if (!initialized) return ERR_NOT_INIT;
    if (!length || length>2048 || (length & 0xff)) return ERR_INVALID_SIZE;
    if (freq!=48000) return ERR_INVALID_FREQ;
    uint32_t format=param & 0xff;
    if (format>7) return ERR_INVALID_FORMAT;
    int first, last;
    if (!port_range(type,&first,&last)) return ERR_INVALID_TYPE;
    static const int channels[8]={1,2,8,1,2,8,8,8};
    pthread_mutex_lock(&table_lock);
    int id=-1;
    for (int i=first;i<=last;++i) if (!ports[i].used) { id=i; break; }
    if (id<0) { pthread_mutex_unlock(&table_lock); return ERR_PORT_FULL; }
    Port *p=&ports[id];
    memset(p,0,sizeof(*p));
    pthread_mutex_init(&p->lock,NULL);
    p->used=1; p->type=type; p->channels=channels[format]; p->is_float=format>=3 && format!=6;
    p->sample_bytes=p->is_float ? 4 : 2; p->frames=(int)length; p->std_layout=format>=6;
    for (int c=0;c<8;++c) p->volume[c]=VOLUME_0DB;
    p->vol_min=p->vol_max=VOLUME_0DB;
    // A warm run (BB_WARM_ONLY) boots the game hidden to compile its pipelines: no output
    // device — the ports below keep the timer sink (writes are paced and dropped), so the
    // warm-up runs silently instead of playing the game out of an invisible window.
    if (!audio_want_device()) {
        printf("Runtime: warm run: port %d keeps the timer sink (no audio device)\n",id);
    } else if (sdl_audio()) {
        SDL_AudioSpec spec={p->is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, p->channels, 48000};
        p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        if (p->stream) {
            SDL_ResumeAudioStreamDevice(p->stream);
            report_audio_device(p,id);
        } else {
            fprintf(stderr,"Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",SDL_GetError(),id);
        }
    }
    p->stats=getenv("BB_AUDIO_STATS")!=NULL;
    const char *dump=getenv("BB_AUDIO_DUMP");
    if (dump) {
        char path[4096];
        snprintf(path,sizeof(path),"%s.port%d.%dch.%s",dump,id,p->channels,p->is_float ? "f32" : "s16");
        p->dump=fopen(path,"wb");
    }
    ++ports_opened;
    pthread_mutex_unlock(&table_lock);
    printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames)\n",id,type,p->channels,p->is_float ? "float" : "s16",length);
    return (type<<16) | id | 0x20000000;
}
static ABI int32_t audio_close(int32_t handle) {
    pthread_mutex_lock(&table_lock);
    int32_t error=0;
    Port *p=port_of(handle,&error);
    if (p) {
        pthread_mutex_lock(&p->lock);
        /* What the device took: bytes queued minus what is still waiting is what CoreAudio
           consumed, which is the only host-side proof that the sink is live. */
        if (p->stream)
            printf("Runtime: audio port %d closed: %llu bytes queued (incl. %llu silence), %d still queued, %u failed puts, starved %u\n",
                   (int)(p-ports),(unsigned long long)p->push_bytes,(unsigned long long)p->silence_bytes,
                   SDL_GetAudioStreamQueued(p->stream),p->put_failed,p->stat_starved);
        if (p->stream) SDL_DestroyAudioStream(p->stream);
        if (p->dump) fclose(p->dump);
        p->dump=NULL;
        p->stream=NULL; p->used=0;
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&table_lock);
    return p ? 0 : error;
}
/* `pace`: wait for this port's next period. sceAudioOutOutputs waits once for all its ports. */
static int32_t output_port(int32_t handle, const void *data, int pace) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    pthread_mutex_lock(&p->lock);
    size_t samples=(size_t)p->frames*(size_t)p->channels, bytes=samples*(size_t)p->sample_bytes;
    uint64_t period=(uint64_t)p->frames*1000000000u/48000u;
    if (data) {
        /* Apply per-channel volume and map PS4 8ch (L R C LFE SL SR BL BR) to SDL 7.1 order. */
        unsigned char converted[2048*8*4];
        static const int remap[8]={0,1,2,3,6,7,4,5};
        for (size_t f=0;f<(size_t)p->frames;++f) for (int c=0;c<p->channels;++c) {
            int target=p->channels==8 && !p->std_layout ? remap[c] : c;
            size_t from=f*(size_t)p->channels+(size_t)c, to=f*(size_t)p->channels+(size_t)target;
            float gain=(float)p->volume[c]/VOLUME_0DB;
            if (p->is_float) { float v; memcpy(&v,(const char *)data+from*4,4); v*=gain; memcpy(converted+to*4,&v,4); }
            else { int16_t v; memcpy(&v,(const char *)data+from*2,2); v=(int16_t)((float)v*gain); memcpy(converted+to*2,&v,2); }
        }
        if (p->dump) fwrite(converted,1,bytes,p->dump);
        if (p->stats) {
            uint64_t now=now_ns();
            if (!p->stat_start_ns) { p->stat_start_ns=now; p->stat_min_queued=1<<30; }
            else if (now-p->stat_last_ns>p->stat_max_gap_ns) p->stat_max_gap_ns=now-p->stat_last_ns;
            p->stat_last_ns=now; ++p->stat_buffers;
            if (p->stream) {
                int queued=SDL_GetAudioStreamQueued(p->stream);
                if (queued<p->stat_min_queued) p->stat_min_queued=queued;
                if (queued==0) ++p->stat_starved;
            }
            if (now-p->stat_start_ns>=5000000000u) {
                double pm_rms=p->pm_samples ? sqrt(p->pm_sumsq/p->pm_samples) : 0;
                printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes, starved %u, volume %d..%d of %d, device buffer peak %.2f dBFS rms %.2f dBFS%s%s\n",
                       (int)(p-ports),p->stat_buffers,(unsigned)(5u*48000u/(unsigned)p->frames),p->stat_max_gap_ns/1e6,
                       p->stat_min_queued,p->stat_starved,p->vol_min,p->vol_max,VOLUME_0DB,
                       20*log10(p->pm_peak>0 ? p->pm_peak : 1e-30),20*log10(pm_rms>0 ? pm_rms : 1e-30),
                       p->stream ? ", stream " : " (timer sink)",
                       p->stream ? (SDL_AudioDevicePaused(SDL_GetAudioStreamDevice(p->stream)) ? "PAUSED" : "running") : "");
                p->stat_start_ns=now; p->stat_max_gap_ns=0; p->stat_starved=0; p->stat_buffers=0; p->stat_min_queued=1<<30;
                p->pm_peak=0; p->pm_sumsq=0; p->pm_samples=0;
            }
        }
        if (pace) {
            uint64_t now=now_ns();
            if (!p->next_deadline_ns || now>p->next_deadline_ns+8*period) p->next_deadline_ns=now; /* start or stall: restart cadence */
            else sleep_until(p->next_deadline_ns);
            p->next_deadline_ns+=period;
        }
        if (p->stream) {
            /* The device drains the queue in quanta (21 ms on PipeWire), so the level
               is a sawtooth: its minimum over ~32 buffers is what gets controlled. */
            int low=2*(int)bytes, queued=SDL_GetAudioStreamQueued(p->stream);
            if (queued<(int)bytes) {
                static const unsigned char silence[2048*8*4];
                int pad=low-queued<(int)bytes ? low-queued : (int)bytes;
                SDL_PutAudioStreamData(p->stream,silence,pad);
                p->silence_bytes+=(uint64_t)pad;
                queued=low;
            }
            if (!p->window_count || queued<p->window_min) p->window_min=queued;
            if (++p->window_count==32) {
                p->adjust_ns=p->window_min>low+2*(int)bytes ? (int64_t)(period/32) : p->window_min<low ? -(int64_t)(period/32) : 0;
                p->window_count=0;
            }
            p->next_deadline_ns+=(uint64_t)p->adjust_ns;
            if (!SDL_PutAudioStreamData(p->stream,converted,(int)bytes)) {
                if (!p->put_failed)
                    fprintf(stderr,"Runtime: audio port %d: SDL_PutAudioStreamData failed (%s); the sink is dead\n",
                            (int)(p-ports),SDL_GetError());
                ++p->put_failed;
            }
            p->push_bytes+=(uint64_t)bytes;
        }
        ++buffers_out;
    }
    p->last_output_us=now_ns()/1000;
    pthread_mutex_unlock(&p->lock);
    return data ? (int32_t)samples : 0;
}
static ABI int32_t audio_output(int32_t handle, const void *data) { return output_port(handle,data,1); }
typedef struct { int32_t handle; const void *data; } OutputParam;
static ABI int32_t audio_outputs(const OutputParam *params, uint32_t count) {
    if (!params) return ERR_INVALID_POINTER;
    if (!count || count>PORTS) return ERR_PORT_FULL;
    int32_t result=0;
    /* One period per call for all ports (FMOD feeds main and BGM together): pacing every
       port would halve the rate and starve both. */
    for (uint32_t i=0;i<count;++i) { int32_t r=output_port(params[i].handle,params[i].data,i==0); if (r<0) return r; result=r; }
    return result;
}
static ABI int32_t audio_volume(int32_t handle, int32_t flags, const int32_t *volume) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!volume) return ERR_INVALID_POINTER;
    pthread_mutex_lock(&p->lock);
    for (int c=0;c<8;++c) if (flags & (1<<c)) {
        if (volume[c]<0 || volume[c]>VOLUME_0DB) { pthread_mutex_unlock(&p->lock); return ERR_INVALID_VOLUME; }
        p->volume[c]=volume[c];
    }
    /* The game's port volume is the last multiplier before the sink (VOLUME_0DB is unity):
     * a quiet game with healthy buffers and a live device is explained here or nowhere. */
    if (p->vol_calls++ < 4) {
        printf("Runtime: audio port %d volume call %u, flags 0x%x:",(int)(p-ports),p->vol_calls,(unsigned)flags);
        for (int c=0;c<8;++c) if (flags & (1<<c)) printf(" ch%d=%d",c,volume[c]);
        printf(" (unity %d)\n",VOLUME_0DB);
    }
    for (int c=0;c<8;++c) {
        if (p->volume[c]<p->vol_min) p->vol_min=p->volume[c];
        if (p->volume[c]>p->vol_max) p->vol_max=p->volume[c];
    }
    pthread_mutex_unlock(&p->lock);
    return 0;
}
static ABI int32_t audio_state(int32_t handle, PortState *state) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!state) return ERR_INVALID_POINTER;
    memset(state,0,sizeof(*state));
    switch (p->type) {
    case 2: case 3: state->output=0x40; state->channel=1; break;          /* headphone */
    case 4: state->output=0x04; state->channel=1; state->volume=127; break; /* pad speaker */
    default: state->output=0x01; state->channel=(uint8_t)(p->channels>2 ? 2 : p->channels); break;
    }
    return 0;
}
static ABI int32_t audio_last_time(int32_t handle, uint64_t *time) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!time) return ERR_INVALID_POINTER;
    *time=p->last_output_us; return 0;
}

static const RuntimeExport exports[]={
    {"sceAudioOutInit",audio_init}, {"sceAudioOutOpen",audio_open}, {"sceAudioOutClose",audio_close},
    {"sceAudioOutOutput",audio_output}, {"sceAudioOutOutputs",audio_outputs},
    {"sceAudioOutSetVolume",audio_volume}, {"sceAudioOutGetPortState",audio_state},
    {"sceAudioOutGetLastOutputTime",audio_last_time},
};
uintptr_t runtime_audio_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_audio_report(void) { printf("Runtime: audio ports opened=%zu, buffers output=%zu\n",ports_opened,buffers_out); }
