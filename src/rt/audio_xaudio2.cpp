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

#define SAMPLE_RATE 44100
#define BLOCK_FRAMES 256
#define BUFFER_COUNT 4

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
static int16_t s_blocks[BUFFER_COUNT][BLOCK_FRAMES * 2];
static int s_next_block;
static int s_inited;
static int s_ok;
static int s_log;
static UINT32 s_min_device_buffers = BUFFER_COUNT;
static UINT32 s_max_device_buffers;
static uint64_t s_last_log_frames;
static LARGE_INTEGER s_start_qpc;
static LARGE_INTEGER s_qpc_frequency;

class VoiceCallback final : public IXAudio2VoiceCallback {
public:
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) noexcept override {}

    void STDMETHODCALLTYPE OnBufferEnd(void *) noexcept override {
        if (s_lock_initialized) {
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

static int submit_block(void) {
    int index = s_next_block;
    EnterCriticalSection(&s_lock);
    sr_audio_mixer_render(&s_mixer, s_blocks[index], BLOCK_FRAMES);
    LeaveCriticalSection(&s_lock);

    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = sizeof(s_blocks[index]);
    buffer.pAudioData = (const BYTE *)s_blocks[index];
    buffer.pContext = (void *)(uintptr_t)index;
    HRESULT result = s_source->SubmitSourceBuffer(&buffer);
    if (FAILED(result)) {
        fprintf(stderr, "audio: SubmitSourceBuffer failed (0x%08lx)\n",
                (unsigned long)result);
        InterlockedExchange(&s_voice_error, (LONG)result);
        return 0;
    }
    s_next_block = (s_next_block + 1) % BUFFER_COUNT;
    return 1;
}

static void fill_device_queue(void) {
    XAUDIO2_VOICE_STATE state = {};
    s_source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued < s_min_device_buffers)
        s_min_device_buffers = state.BuffersQueued;
    if (state.BuffersQueued > s_max_device_buffers)
        s_max_device_buffers = state.BuffersQueued;
    while (state.BuffersQueued < BUFFER_COUNT && !s_voice_error) {
        if (!submit_block()) break;
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
    fprintf(
        stderr,
        "audio: XAudio2 played=%llu device=%u blocks/%u frames (%.2f ms) "
        "max_guest_queue=%llu dropped=%llu resynced=%llu\n",
        (unsigned long long)stats.completed_frames,
        state.BuffersQueued,
        state.BuffersQueued * BLOCK_FRAMES,
        state.BuffersQueued * BLOCK_FRAMES * 1000.0 / SAMPLE_RATE,
        (unsigned long long)stats.max_queued_frames,
        (unsigned long long)stats.dropped_frames,
        (unsigned long long)stats.resynced_frames);
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
    s_next_block = 0;
    s_min_device_buffers = BUFFER_COUNT;
    s_max_device_buffers = 0;
    s_last_log_frames = 0;
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
        fill_device_queue();
        s_min_device_buffers = BUFFER_COUNT;
        s_max_device_buffers = BUFFER_COUNT;
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
    if (!sr_audio_init() || frames <= 0) return;
    EnterCriticalSection(&s_lock);
    sr_audio_mixer_push(
        &s_mixer, channel, stereo, frames, volume_left, volume_right);
    LeaveCriticalSection(&s_lock);
}

extern "C" int sr_audio_queued(int channel) {
    if (!s_ok || s_voice_error) return -1;
    EnterCriticalSection(&s_lock);
    int queued = sr_audio_mixer_queued(&s_mixer, channel);
    LeaveCriticalSection(&s_lock);
    return queued;
}

extern "C" int sr_audio_device_latency_frames(void) {
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
            "max_guest_queue=%llu, dropped=%llu, resynced=%llu, silent_blocks=%llu%s\n",
            (unsigned long long)stats.completed_frames,
            played_ms, played_ms - elapsed_ms,
            s_min_device_buffers, s_max_device_buffers,
            (unsigned long long)stats.max_queued_frames,
            (unsigned long long)stats.dropped_frames,
            (unsigned long long)stats.resynced_frames,
            (unsigned long long)stats.silent_blocks,
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
