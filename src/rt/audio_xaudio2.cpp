/* Low-latency Windows audio sink: shared PSP-channel mixer drained by XAudio2 2.9. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>

#include "audio.h"
#include "audio_mixer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" int sched_fast_forwarding(void);

#define SAMPLE_RATE 44100
#define BLOCK_FRAMES 256
#define BUFFER_COUNT 8
#define PADDING_FADE_FRAMES 64
#define RESUME_PREBUFFER_FRAMES (BUFFER_COUNT * BLOCK_FRAMES * 2)
#define RESUME_MAX_WAIT_MS 500

static SrAudioMixer s_mixer;
static CRITICAL_SECTION s_lock;
static int s_lock_initialized;
static IXAudio2 *s_engine;
static IXAudio2MasteringVoice *s_master;
static IXAudio2SourceVoice *s_source;
static HANDLE s_event;
static HANDLE s_thread;
static volatile LONG s_run;
static volatile LONG s_voice_error;
static volatile LONG s_real_device_buffers;
static int16_t s_blocks[BUFFER_COUNT][BLOCK_FRAMES * 2];
static int s_next_block;
static int s_inited;
static int s_ok;
static int s_log;
static UINT32 s_min_device_buffers = BUFFER_COUNT;
static UINT32 s_max_device_buffers;
static uint64_t s_last_log_frames;
static uint64_t s_padding_blocks;
static uint64_t s_padding_transitions;
static int s_padding_active;
static int16_t s_last_left;
static int16_t s_last_right;
static LARGE_INTEGER s_pending_audio_qpc;
static LARGE_INTEGER s_start_qpc;
static LARGE_INTEGER s_qpc_frequency;
static int s_playback_started;
static uint64_t s_underruns;       /* times the device queue drained after playback started */

class VoiceCallback final : public IXAudio2VoiceCallback {
public:
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) noexcept override {}

    void STDMETHODCALLTYPE OnBufferEnd(void *context) noexcept override {
        if (context && s_lock_initialized) {
            InterlockedDecrement(&s_real_device_buffers);
            EnterCriticalSection(&s_lock);
            sr_audio_mixer_complete(&s_mixer, BLOCK_FRAMES);
            LeaveCriticalSection(&s_lock);
        }
        if (s_event) SetEvent(s_event);
    }

    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT error) noexcept override {
        InterlockedExchange(&s_voice_error, (LONG)error);
        if (s_event) SetEvent(s_event);
    }
};

static VoiceCallback s_callback;

static int submit_block(int guest_audio) {
    int index = s_next_block;
    if (guest_audio) {
        EnterCriticalSection(&s_lock);
        sr_audio_mixer_render(
            &s_mixer, s_blocks[index], BLOCK_FRAMES);
        LeaveCriticalSection(&s_lock);
        if (s_padding_active) {
            for (int i = 0; i < PADDING_FADE_FRAMES; i++) {
                s_blocks[index][i * 2] =
                    (int16_t)(
                        (int32_t)s_blocks[index][i * 2] *
                        (i + 1) / PADDING_FADE_FRAMES);
                s_blocks[index][i * 2 + 1] =
                    (int16_t)(
                        (int32_t)s_blocks[index][i * 2 + 1] *
                        (i + 1) / PADDING_FADE_FRAMES);
            }
            s_padding_active = 0;
            s_pending_audio_qpc.QuadPart = 0;
        }
        s_last_left =
            s_blocks[index][(BLOCK_FRAMES - 1) * 2];
        s_last_right =
            s_blocks[index][(BLOCK_FRAMES - 1) * 2 + 1];
    } else {
        memset(s_blocks[index], 0, sizeof(s_blocks[index]));
        if (!s_padding_active) {
            for (int i = 0; i < PADDING_FADE_FRAMES; i++) {
                int scale = PADDING_FADE_FRAMES - i - 1;
                s_blocks[index][i * 2] =
                    (int16_t)(
                        (int32_t)s_last_left * scale /
                        PADDING_FADE_FRAMES);
                s_blocks[index][i * 2 + 1] =
                    (int16_t)(
                        (int32_t)s_last_right * scale /
                        PADDING_FADE_FRAMES);
            }
            s_padding_active = 1;
            s_padding_transitions++;
        }
    }

    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = sizeof(s_blocks[index]);
    buffer.pAudioData = (const BYTE *)s_blocks[index];
    buffer.pContext =
        guest_audio ? (void *)(uintptr_t)(index + 1) : nullptr;
    if (guest_audio)
        InterlockedIncrement(&s_real_device_buffers);
    HRESULT result = s_source->SubmitSourceBuffer(&buffer);
    if (FAILED(result)) {
        if (guest_audio)
            InterlockedDecrement(&s_real_device_buffers);
        fprintf(stderr, "audio: SubmitSourceBuffer failed (0x%08lx)\n",
                (unsigned long)result);
        InterlockedExchange(&s_voice_error, (LONG)result);
        return 0;
    }
    if (!guest_audio) s_padding_blocks++;
    s_next_block = (s_next_block + 1) % BUFFER_COUNT;
    return 1;
}

static int guest_audio_ready(int available) {
    if (available < BLOCK_FRAMES) return 0;
    if (!s_padding_active) {
        s_pending_audio_qpc.QuadPart = 0;
        return 1;
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (!s_pending_audio_qpc.QuadPart)
        s_pending_audio_qpc = now;
    if (available >= RESUME_PREBUFFER_FRAMES)
        return 1;
    return
        now.QuadPart - s_pending_audio_qpc.QuadPart >=
        s_qpc_frequency.QuadPart * RESUME_MAX_WAIT_MS / 1000;
}

static void fill_device_queue(void) {
    XAUDIO2_VOICE_STATE state = {};
    s_source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (s_playback_started &&
        state.BuffersQueued < s_min_device_buffers) {
        s_min_device_buffers = state.BuffersQueued;
    }
    if (s_playback_started && state.BuffersQueued == 0) {
        s_playback_started = 0;     /* one underrun per drain; the next submit re-arms it */
        s_underruns++;
        if (s_log) {
            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            fprintf(stderr, "audio: XAudio2 underrun #%llu at %.3f s\n",
                    (unsigned long long)s_underruns,
                    (now.QuadPart - s_start_qpc.QuadPart) /
                        (double)s_qpc_frequency.QuadPart);
        }
    }
    if (state.BuffersQueued > s_max_device_buffers)
        s_max_device_buffers = state.BuffersQueued;
    while (state.BuffersQueued < BUFFER_COUNT && !s_voice_error) {
        EnterCriticalSection(&s_lock);
        int available = sr_audio_mixer_available(&s_mixer);
        LeaveCriticalSection(&s_lock);
        int guest_audio = guest_audio_ready(available);
        if (!guest_audio && !s_playback_started) break;
        if (!submit_block(guest_audio)) break;
        s_playback_started = 1;
        state.BuffersQueued++;
        if (state.BuffersQueued > s_max_device_buffers)
            s_max_device_buffers = state.BuffersQueued;
    }
}

static void log_progress(void) {
    if (!s_log) return;
    SrAudioMixerStats stats;
    EnterCriticalSection(&s_lock);
    stats = *sr_audio_mixer_stats(&s_mixer);
    LeaveCriticalSection(&s_lock);
    if (stats.completed_frames - s_last_log_frames < SAMPLE_RATE * 3u)
        return;
    s_last_log_frames = stats.completed_frames;

    XAUDIO2_VOICE_STATE state = {};
    s_source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    double elapsed_ms =
        (now.QuadPart - s_start_qpc.QuadPart) * 1000.0 /
        s_qpc_frequency.QuadPart;
    double played_ms = stats.completed_frames * 1000.0 / SAMPLE_RATE;
    fprintf(
        stderr,
        "audio: XAudio2 played=%llu device=%u blocks/%u frames (%.2f ms) "
        "clock_drift=%.2f ms max_guest_queue=%llu dropped=%llu "
        "resynced=%llu silent_blocks=%llu underruns=%llu "
        "padding_blocks=%llu padding_transitions=%llu "
        "real_buffers=%ld\n",
        (unsigned long long)stats.completed_frames,
        state.BuffersQueued,
        state.BuffersQueued * BLOCK_FRAMES,
        state.BuffersQueued * BLOCK_FRAMES * 1000.0 / SAMPLE_RATE,
        played_ms - elapsed_ms,
        (unsigned long long)stats.max_queued_frames,
        (unsigned long long)stats.dropped_frames,
        (unsigned long long)stats.resynced_frames,
        (unsigned long long)stats.silent_blocks,
        (unsigned long long)s_underruns,
        (unsigned long long)s_padding_blocks,
        (unsigned long long)s_padding_transitions,
        (long)InterlockedCompareExchange(
            &s_real_device_buffers, 0, 0));
}

static DWORD WINAPI feeder(void *) {
    while (InterlockedCompareExchange(&s_run, 1, 1)) {
        fill_device_queue();
        log_progress();
        WaitForSingleObject(s_event, 100);
        if (s_voice_error) break;
    }
    return 0;
}

static void release_backend(void) {
    if (s_source) {
        s_source->Stop(0);
        s_source->FlushSourceBuffers();
        s_source->DestroyVoice();
        s_source = nullptr;
    }
    if (s_master) {
        s_master->DestroyVoice();
        s_master = nullptr;
    }
    if (s_engine) {
        s_engine->StopEngine();
        s_engine->Release();
        s_engine = nullptr;
    }
    if (s_thread) {
        CloseHandle(s_thread);
        s_thread = nullptr;
    }
    if (s_event) {
        CloseHandle(s_event);
        s_event = nullptr;
    }
}

extern "C" int sr_audio_init(void) {
    if (s_inited) return s_ok;
    s_inited = 1;
    s_log = getenv("SR_AUDIO_LOG") ? 1 : 0;
    if (getenv("SR_NOAUDIO")) return 0;

    InterlockedExchange(&s_voice_error, 0);
    InterlockedExchange(&s_real_device_buffers, 0);
    s_next_block = 0;
    s_min_device_buffers = BUFFER_COUNT;
    s_max_device_buffers = 0;
    s_last_log_frames = 0;
    s_padding_blocks = 0;
    s_padding_transitions = 0;
    s_padding_active = 0;
    s_last_left = 0;
    s_last_right = 0;
    s_pending_audio_qpc.QuadPart = 0;
    s_playback_started = 0;
    s_underruns = 0;
    sr_audio_mixer_init(&s_mixer);
    InitializeCriticalSection(&s_lock);
    s_lock_initialized = 1;
    QueryPerformanceFrequency(&s_qpc_frequency);
    QueryPerformanceCounter(&s_start_qpc);

    HRESULT result = XAudio2Create(&s_engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(result))
        result = s_engine->CreateMasteringVoice(&s_master);

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = SAMPLE_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = SAMPLE_RATE * 4;
    if (SUCCEEDED(result))
        result = s_engine->CreateSourceVoice(
            &s_source, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO,
            &s_callback, nullptr, nullptr);

    s_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(result) && !s_event) result = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(result)) {
        result = s_source->Start(0);
        QueryPerformanceCounter(&s_start_qpc);
    }
    if (SUCCEEDED(result)) {
        InterlockedExchange(&s_run, 1);
        s_thread = CreateThread(nullptr, 0, feeder, nullptr, 0, nullptr);
        if (!s_thread) result = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(result)) {
        fprintf(stderr, "audio: XAudio2 initialization failed (0x%08lx)\n",
                (unsigned long)result);
        InterlockedExchange(&s_run, 0);
        release_backend();
        DeleteCriticalSection(&s_lock);
        s_lock_initialized = 0;
        return 0;
    }

    atexit(sr_audio_shutdown);
    s_ok = 1;
    fprintf(
        stderr,
        "audio: XAudio2 44.1 kHz stereo, %d x %d-frame buffers (%.2f ms)\n",
        BUFFER_COUNT, BLOCK_FRAMES,
        BUFFER_COUNT * BLOCK_FRAMES * 1000.0 / SAMPLE_RATE);
    return 1;
}

extern "C" void sr_audio_push(
    int channel, const int16_t *stereo, int frames,
    int volume_left, int volume_right) {
    if (sched_fast_forwarding()) return;
    if (!sr_audio_init() || frames <= 0) return;
    EnterCriticalSection(&s_lock);
    sr_audio_mixer_push(
        &s_mixer, channel, stereo, frames, volume_left, volume_right);
    LeaveCriticalSection(&s_lock);
    if (s_event) SetEvent(s_event);
}

extern "C" int sr_audio_queued(int channel) {
    if (sched_fast_forwarding()) return -1;
    if (!s_ok || s_voice_error) return -1;
    EnterCriticalSection(&s_lock);
    int queued = sr_audio_mixer_queued(&s_mixer, channel);
    LeaveCriticalSection(&s_lock);
    return queued;
}

extern "C" int sr_audio_device_latency_frames(void) {
    if (sched_fast_forwarding()) return -1;
    if (!s_ok || !s_source) return -1;
    XAUDIO2_VOICE_STATE state = {};
    s_source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)state.BuffersQueued * BLOCK_FRAMES;
}

extern "C" const char *sr_audio_backend_name(void) {
    return "XAudio2";
}

extern "C" void sr_audio_shutdown(void) {
    if (!s_inited) return;
    InterlockedExchange(&s_run, 0);
    if (s_event) SetEvent(s_event);
    if (s_thread) WaitForSingleObject(s_thread, 2000);

    SrAudioMixerStats stats = {};
    if (s_lock_initialized) {
        EnterCriticalSection(&s_lock);
        stats = *sr_audio_mixer_stats(&s_mixer);
        LeaveCriticalSection(&s_lock);
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    double elapsed_ms = s_qpc_frequency.QuadPart
        ? (now.QuadPart - s_start_qpc.QuadPart) * 1000.0 /
            s_qpc_frequency.QuadPart
        : 0.0;
    double played_ms = stats.completed_frames * 1000.0 / SAMPLE_RATE;
    if (s_ok || s_log) {
        fprintf(
            stderr,
            "audio: XAudio2 shutdown played=%llu frames (%.2f ms), "
            "clock_drift=%.2f ms, device_buffers=%u..%u, "
            "max_guest_queue=%llu, dropped=%llu, resynced=%llu, silent_blocks=%llu, "
            "underruns=%llu, padding_blocks=%llu, "
            "padding_transitions=%llu%s\n",
            (unsigned long long)stats.completed_frames,
            played_ms, played_ms - elapsed_ms,
            s_playback_started ? s_min_device_buffers : 0u,
            s_max_device_buffers,
            (unsigned long long)stats.max_queued_frames,
            (unsigned long long)stats.dropped_frames,
            (unsigned long long)stats.resynced_frames,
            (unsigned long long)stats.silent_blocks,
            (unsigned long long)s_underruns,
            (unsigned long long)s_padding_blocks,
            (unsigned long long)s_padding_transitions,
            s_voice_error ? ", voice_error" : "");
    }

    release_backend();
    if (s_lock_initialized) {
        DeleteCriticalSection(&s_lock);
        s_lock_initialized = 0;
    }
    s_ok = 0;
    s_inited = 0;
}
