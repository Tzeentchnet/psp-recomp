#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "pipeline.h"

#include "device.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIPELINE_CACHE_CAPACITY 512u
#define PIPELINE_CACHE_PATH_CAPACITY 1024u
#define PIPELINE_LIBRARY_MAX_BYTES (64u * 1024u * 1024u)
#define PIPELINE_LIBRARY_VERSION 2u
#define PIPELINE_NAME_CAPACITY 48u

enum PipelineEntryState {
    PIPELINE_ENTRY_FREE,
    PIPELINE_ENTRY_PENDING,
    PIPELINE_ENTRY_READY,
    PIPELINE_ENTRY_FAILED,
};

struct PipelineEntry {
    GeD3D12PipelineKey key;
    uint64_t hash;
    ID3D12PipelineState *pipeline;
    PipelineEntryState state;
};

struct PipelineLibraryHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t shader_hash;
    uint32_t key_size;
    uint32_t adapter_low;
    int32_t adapter_high;
    uint32_t library_size;
};

struct PipelineBuildResult {
    ID3D12PipelineState *pipeline;
    bool library_hit;
    bool compiled;
    bool cache_write;
};

static const char k_library_magic[8] = {
    'S', 'R', 'D', '1', '2', 'P', 'L', '\0',
};

static PipelineEntry s_entries[PIPELINE_CACHE_CAPACITY];
static UINT s_queue[PIPELINE_CACHE_CAPACITY];
static UINT s_queue_read;
static UINT s_queue_write;
static UINT s_queue_count;
static GeD3D12PipelineStats s_stats;
static GeD3D12PipelineKey s_uber_key;
static ID3D12PipelineState *s_uber_pipeline;
static ID3D12PipelineLibrary *s_library;
static CRITICAL_SECTION s_lock;
static HANDLE s_worker;
static HANDLE s_worker_event;
static char s_cache_path[PIPELINE_CACHE_PATH_CAPACITY];
static int s_lock_initialized;
static int s_stopping;
static int s_library_dirty;
static int s_ready;
static int s_log;

static_assert(
    sizeof(PipelineLibraryHeader) == 40,
    "D3D12 pipeline-library header ABI changed");

static int env_enabled(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static int is_path_separator(char value) {
    return value == '\\' || value == '/';
}

static bool create_directory_if_needed(const char *path) {
    if (CreateDirectoryA(path, nullptr)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        DWORD attributes = GetFileAttributesA(path);
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return true;
        }
    }
    fprintf(stderr,
            "ged3d12: create PSO cache directory failed "
            "(path=%s, error=%lu)\n",
            path, (unsigned long)error);
    return false;
}

static bool ensure_parent_directories(const char *path) {
    size_t length = strlen(path);
    if (!length || length >= sizeof(s_cache_path)) return false;

    char parent[PIPELINE_CACHE_PATH_CAPACITY];
    memcpy(parent, path, length + 1);
    size_t parent_end = length;
    while (parent_end && !is_path_separator(parent[parent_end - 1]))
        parent_end--;
    if (!parent_end) return true;
    while (parent_end && is_path_separator(parent[parent_end - 1]))
        parent_end--;
    parent[parent_end] = '\0';
    if (!parent[0]) return true;

    size_t start = 0;
    if (parent_end >= 3 && parent[1] == ':' &&
        is_path_separator(parent[2])) {
        start = 3;
    } else if (parent_end >= 2 &&
               is_path_separator(parent[0]) &&
               is_path_separator(parent[1])) {
        unsigned separators = 0;
        for (size_t i = 2; i < parent_end; i++) {
            if (!is_path_separator(parent[i])) continue;
            separators++;
            if (separators == 2) {
                start = i + 1;
                break;
            }
        }
    }

    for (size_t i = start; i <= parent_end; i++) {
        if (i != parent_end && !is_path_separator(parent[i])) continue;
        char saved = parent[i];
        parent[i] = '\0';
        bool ok = !parent[0] || create_directory_if_needed(parent);
        parent[i] = saved;
        if (!ok) return false;
    }
    return true;
}

static bool configure_cache_path(void) {
    const char *override_path = getenv("SR_D3D12_PSO_CACHE");
    if (override_path && strcmp(override_path, "0") == 0) {
        if (s_log) fprintf(stderr, "ged3d12: PSO disk cache disabled\n");
        return false;
    }

    int count;
    if (override_path && override_path[0]) {
        count = snprintf(
            s_cache_path, sizeof(s_cache_path), "%s", override_path);
    } else {
        const char *local = getenv("LOCALAPPDATA");
        if (!local || !local[0]) {
            fprintf(stderr,
                    "ged3d12: LOCALAPPDATA is unset; "
                    "set SR_D3D12_PSO_CACHE to enable the PSO disk cache\n");
            return false;
        }
        count = snprintf(
            s_cache_path, sizeof(s_cache_path),
            "%s\\KZLRecomp\\cache\\d3d12-pipelines.bin", local);
    }
    if (count < 0 || (size_t)count >= sizeof(s_cache_path)) {
        fprintf(stderr, "ged3d12: PSO cache path is too long\n");
        s_cache_path[0] = '\0';
        return false;
    }
    if (!ensure_parent_directories(s_cache_path)) {
        s_cache_path[0] = '\0';
        return false;
    }
    return true;
}

static uint64_t hash_bytes(
    uint64_t hash, const void *memory, size_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(memory);
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t pipeline_shader_hash(void) {
    uint64_t hash = UINT64_C(14695981039346656037);
    uint32_t version = PIPELINE_LIBRARY_VERSION;
    uint32_t key_size = sizeof(GeD3D12PipelineKey);
    hash = hash_bytes(hash, &version, sizeof(version));
    hash = hash_bytes(hash, &key_size, sizeof(key_size));
    D3D12_SHADER_BYTECODE vertex = ged3d12_vertex_shader();
    D3D12_SHADER_BYTECODE pixel = ged3d12_pixel_shader();
    hash = hash_bytes(hash, vertex.pShaderBytecode, vertex.BytecodeLength);
    return hash_bytes(hash, pixel.pShaderBytecode, pixel.BytecodeLength);
}

static void *read_library_blob(SIZE_T *blob_size) {
    *blob_size = 0;
    if (!s_cache_path[0]) return nullptr;

    FILE *file = fopen(s_cache_path, "rb");
    if (!file) {
        if (errno != ENOENT) {
            fprintf(stderr,
                    "ged3d12: open PSO cache failed "
                    "(path=%s, error=%s)\n",
                    s_cache_path, strerror(errno));
        } else if (s_log) {
            fprintf(stderr, "ged3d12: PSO disk cache is cold\n");
        }
        return nullptr;
    }

    PipelineLibraryHeader header = {};
    bool valid =
        fread(&header, 1, sizeof(header), file) == sizeof(header);
    LUID adapter = ged3d12_device()->GetAdapterLuid();
    valid = valid &&
        memcmp(header.magic, k_library_magic, sizeof(header.magic)) == 0 &&
        header.version == PIPELINE_LIBRARY_VERSION &&
        header.header_size == sizeof(header) &&
        header.shader_hash == pipeline_shader_hash() &&
        header.key_size == sizeof(GeD3D12PipelineKey) &&
        header.adapter_low == adapter.LowPart &&
        header.adapter_high == adapter.HighPart &&
        header.library_size > 0 &&
        header.library_size <= PIPELINE_LIBRARY_MAX_BYTES;
    if (!valid) {
        fprintf(stderr,
                "ged3d12: ignoring incompatible or corrupt PSO cache %s\n",
                s_cache_path);
        fclose(file);
        return nullptr;
    }

    void *blob = malloc(header.library_size);
    if (!blob) {
        fprintf(stderr,
                "ged3d12: allocate PSO cache blob failed (%u bytes)\n",
                header.library_size);
        fclose(file);
        return nullptr;
    }
    valid =
        fread(blob, 1, header.library_size, file) == header.library_size &&
        fgetc(file) == EOF;
    if (fclose(file) != 0) valid = false;
    if (!valid) {
        fprintf(stderr, "ged3d12: read PSO cache failed (%s)\n", s_cache_path);
        free(blob);
        return nullptr;
    }
    *blob_size = header.library_size;
    return blob;
}

static void initialize_pipeline_library(void) {
    ID3D12Device1 *device = nullptr;
    HRESULT hr = ged3d12_device()->QueryInterface(IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        fprintf(stderr,
                "ged3d12: ID3D12Device1 is unavailable "
                "(HRESULT 0x%08lx); continuing without disk persistence\n",
                (unsigned long)hr);
        s_cache_path[0] = '\0';
        return;
    }

    SIZE_T blob_size = 0;
    void *blob = read_library_blob(&blob_size);
    hr = device->CreatePipelineLibrary(
        blob, blob_size, IID_PPV_ARGS(&s_library));
    if (FAILED(hr) && blob) {
        fprintf(stderr,
                "ged3d12: cached pipeline library was rejected "
                "(HRESULT 0x%08lx); rebuilding it\n",
                (unsigned long)hr);
        hr = device->CreatePipelineLibrary(
            nullptr, 0, IID_PPV_ARGS(&s_library));
    }
    free(blob);
    device->Release();

    if (FAILED(hr)) {
        fprintf(stderr,
                "ged3d12: pipeline libraries are unavailable "
                "(HRESULT 0x%08lx); continuing without disk persistence\n",
                (unsigned long)hr);
        s_library = nullptr;
        s_cache_path[0] = '\0';
        return;
    }
    if (s_log && blob_size) {
        fprintf(stderr,
                "ged3d12: loaded %llu-byte PSO disk cache %s\n",
                (unsigned long long)blob_size, s_cache_path);
    }
}

static void pipeline_name(
    const GeD3D12PipelineKey *key,
    wchar_t name[PIPELINE_NAME_CAPACITY]) {
    static const wchar_t hex[] = L"0123456789abcdef";
    static const wchar_t prefix[] = L"sr-pso-v1-";
    size_t offset = 0;
    for (; prefix[offset]; offset++) name[offset] = prefix[offset];
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(key);
    for (size_t i = 0; i < sizeof(*key); i++) {
        name[offset++] = hex[bytes[i] >> 4];
        name[offset++] = hex[bytes[i] & 15];
    }
    name[offset] = L'\0';
}

static D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc(
    const GeD3D12PipelineKey *key) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = ged3d12_root_signature();
    desc.VS = ged3d12_vertex_shader();
    desc.PS = ged3d12_pixel_shader();

    D3D12_RENDER_TARGET_BLEND_DESC *blend =
        &desc.BlendState.RenderTarget[0];
    blend->BlendEnable = key->blend_enable;
    blend->LogicOpEnable = FALSE;
    blend->SrcBlend = (D3D12_BLEND)key->source_blend;
    blend->DestBlend = (D3D12_BLEND)key->destination_blend;
    blend->BlendOp = (D3D12_BLEND_OP)key->blend_operation;
    blend->SrcBlendAlpha = (D3D12_BLEND)key->source_alpha_blend;
    blend->DestBlendAlpha = (D3D12_BLEND)key->destination_alpha_blend;
    blend->BlendOpAlpha = (D3D12_BLEND_OP)key->alpha_blend_operation;
    blend->LogicOp = D3D12_LOGIC_OP_NOOP;
    blend->RenderTargetWriteMask = key->color_write_mask;

    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = (D3D12_CULL_MODE)key->cull_mode;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.DepthStencilState.DepthEnable = key->depth_enable;
    desc.DepthStencilState.DepthWriteMask = key->depth_write
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc =
        (D3D12_COMPARISON_FUNC)key->depth_function;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DepthStencilState.StencilReadMask =
        D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask =
        D3D12_DEFAULT_STENCIL_WRITE_MASK;

    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = (DXGI_FORMAT)key->render_target_format;
    desc.DSVFormat = (DXGI_FORMAT)key->depth_stencil_format;
    desc.SampleDesc.Count = 1;
    return desc;
}

static ID3D12PipelineState *create_pipeline(
    const GeD3D12PipelineKey *key,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc) {
    ID3D12PipelineState *pipeline = nullptr;
    HRESULT hr = ged3d12_device()->CreateGraphicsPipelineState(
        desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) {
        fprintf(stderr,
                "ged3d12: create GE graphics pipeline failed "
                "(hash=%016llx, HRESULT 0x%08lx)\n",
                (unsigned long long)ged3d12_pipeline_key_hash(key),
                (unsigned long)hr);
        return nullptr;
    }
    return pipeline;
}

static bool store_pipeline(
    const GeD3D12PipelineKey *key,
    ID3D12PipelineState *pipeline) {
    if (!s_library) return false;
    wchar_t name[PIPELINE_NAME_CAPACITY];
    pipeline_name(key, name);
    HRESULT hr = s_library->StorePipeline(name, pipeline);
    if (FAILED(hr)) {
        fprintf(stderr,
                "ged3d12: store PSO in pipeline library failed "
                "(hash=%016llx, HRESULT 0x%08lx)\n",
                (unsigned long long)ged3d12_pipeline_key_hash(key),
                (unsigned long)hr);
        return false;
    }
    s_library_dirty = 1;
    return true;
}

static bool write_all(HANDLE file, const void *memory, DWORD size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(memory);
    DWORD offset = 0;
    while (offset < size) {
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, size - offset,
                       &written, nullptr) ||
            !written) {
            return false;
        }
        offset += written;
    }
    return true;
}

static bool save_pipeline_library(void) {
    if (!s_library || !s_library_dirty || !s_cache_path[0])
        return false;

    SIZE_T serialized_size = s_library->GetSerializedSize();
    if (!serialized_size ||
        serialized_size > PIPELINE_LIBRARY_MAX_BYTES ||
        serialized_size > UINT32_MAX) {
        fprintf(stderr,
                "ged3d12: invalid serialized PSO cache size (%llu bytes)\n",
                (unsigned long long)serialized_size);
        return false;
    }
    void *blob = malloc(serialized_size);
    if (!blob) {
        fprintf(stderr,
                "ged3d12: allocate serialized PSO cache failed "
                "(%llu bytes)\n",
                (unsigned long long)serialized_size);
        return false;
    }
    HRESULT hr = s_library->Serialize(blob, serialized_size);
    if (FAILED(hr)) {
        fprintf(stderr,
                "ged3d12: serialize PSO cache failed "
                "(HRESULT 0x%08lx)\n",
                (unsigned long)hr);
        free(blob);
        return false;
    }

    PipelineLibraryHeader header = {};
    memcpy(header.magic, k_library_magic, sizeof(header.magic));
    header.version = PIPELINE_LIBRARY_VERSION;
    header.header_size = sizeof(header);
    header.shader_hash = pipeline_shader_hash();
    header.key_size = sizeof(GeD3D12PipelineKey);
    LUID adapter = ged3d12_device()->GetAdapterLuid();
    header.adapter_low = adapter.LowPart;
    header.adapter_high = adapter.HighPart;
    header.library_size = (uint32_t)serialized_size;

    char temporary[PIPELINE_CACHE_PATH_CAPACITY + 32];
    int count = snprintf(
        temporary, sizeof(temporary), "%s.tmp.%lu",
        s_cache_path, (unsigned long)GetCurrentProcessId());
    if (count < 0 || (size_t)count >= sizeof(temporary)) {
        fprintf(stderr, "ged3d12: temporary PSO cache path is too long\n");
        free(blob);
        return false;
    }
    HANDLE file = CreateFileA(
        temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr,
                "ged3d12: create temporary PSO cache failed "
                "(path=%s, error=%lu)\n",
                temporary, (unsigned long)GetLastError());
        free(blob);
        return false;
    }

    bool written =
        write_all(file, &header, sizeof(header)) &&
        write_all(file, blob, (DWORD)serialized_size) &&
        FlushFileBuffers(file);
    DWORD write_error = written ? ERROR_SUCCESS : GetLastError();
    if (!CloseHandle(file) && written) {
        written = false;
        write_error = GetLastError();
    }
    free(blob);
    if (!written) {
        fprintf(stderr,
                "ged3d12: write PSO cache failed "
                "(path=%s, error=%lu)\n",
                temporary, (unsigned long)write_error);
        DeleteFileA(temporary);
        return false;
    }
    if (!MoveFileExA(
            temporary, s_cache_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        fprintf(stderr,
                "ged3d12: publish PSO cache failed "
                "(path=%s, error=%lu)\n",
                s_cache_path, (unsigned long)error);
        DeleteFileA(temporary);
        return false;
    }
    s_library_dirty = 0;
    if (s_log) {
        fprintf(stderr,
                "ged3d12: wrote %llu-byte PSO disk cache %s\n",
                (unsigned long long)serialized_size, s_cache_path);
    }
    return true;
}

static PipelineBuildResult load_or_create_pipeline(
    const GeD3D12PipelineKey *key) {
    PipelineBuildResult result = {};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = pipeline_desc(key);
    if (s_library) {
        wchar_t name[PIPELINE_NAME_CAPACITY];
        pipeline_name(key, name);
        HRESULT hr = s_library->LoadGraphicsPipeline(
            name, &desc, IID_PPV_ARGS(&result.pipeline));
        if (SUCCEEDED(hr)) {
            result.library_hit = true;
            return result;
        }
        if (hr != E_INVALIDARG && s_log) {
            fprintf(stderr,
                    "ged3d12: load PSO from pipeline library missed "
                    "(hash=%016llx, HRESULT 0x%08lx)\n",
                    (unsigned long long)ged3d12_pipeline_key_hash(key),
                    (unsigned long)hr);
        }
    }

    result.pipeline = create_pipeline(key, &desc);
    if (!result.pipeline) return result;
    result.compiled = true;
    if (store_pipeline(key, result.pipeline))
        result.cache_write = save_pipeline_library();
    return result;
}

static DWORD WINAPI pipeline_worker(void *) {
    for (;;) {
        UINT index = UINT_MAX;
        GeD3D12PipelineKey key = {};
        EnterCriticalSection(&s_lock);
        if (s_queue_count) {
            index = s_queue[s_queue_read];
            s_queue_read =
                (s_queue_read + 1) & (PIPELINE_CACHE_CAPACITY - 1);
            s_queue_count--;
            key = s_entries[index].key;
        } else if (s_stopping) {
            LeaveCriticalSection(&s_lock);
            break;
        }
        LeaveCriticalSection(&s_lock);

        if (index == UINT_MAX) {
            DWORD wait = WaitForSingleObject(s_worker_event, INFINITE);
            if (wait != WAIT_OBJECT_0) {
                fprintf(stderr,
                        "ged3d12: PSO worker wait failed "
                        "(wait=%lu, error=%lu)\n",
                        (unsigned long)wait,
                        (unsigned long)GetLastError());
                EnterCriticalSection(&s_lock);
                s_stats.failures++;
                s_stopping = 1;
                LeaveCriticalSection(&s_lock);
            }
            continue;
        }

        PipelineBuildResult result = load_or_create_pipeline(&key);
        EnterCriticalSection(&s_lock);
        PipelineEntry *entry = &s_entries[index];
        if (result.pipeline) {
            entry->pipeline = result.pipeline;
            entry->state = PIPELINE_ENTRY_READY;
            if (result.library_hit) s_stats.library_hits++;
            if (result.compiled) s_stats.compiled++;
            if (result.cache_write) s_stats.cache_writes++;
        } else {
            entry->state = PIPELINE_ENTRY_FAILED;
            s_stats.failures++;
        }
        if (s_stats.pending) s_stats.pending--;
        LeaveCriticalSection(&s_lock);

        if (s_log) {
            fprintf(
                stderr,
                "ged3d12: async GE PSO hash=%016llx source=%s\n",
                (unsigned long long)ged3d12_pipeline_key_hash(&key),
                result.library_hit ? "library" :
                result.pipeline ? "compiler" : "failed");
        }
    }
    return 0;
}

static bool queue_pipeline_locked(
    const GeD3D12PipelineKey *key, uint64_t hash,
    UINT index, bool count_miss) {
    if (index == UINT_MAX ||
        s_queue_count >= PIPELINE_CACHE_CAPACITY) {
        return false;
    }
    PipelineEntry *entry = &s_entries[index];
    entry->key = *key;
    entry->hash = hash;
    entry->state = PIPELINE_ENTRY_PENDING;
    s_queue[s_queue_write] = index;
    s_queue_write =
        (s_queue_write + 1) & (PIPELINE_CACHE_CAPACITY - 1);
    s_queue_count++;
    s_stats.entries++;
    s_stats.pending++;
    if (count_miss) s_stats.misses++;
    return true;
}

static void release_pipeline_objects(void) {
    for (UINT i = 0; i < PIPELINE_CACHE_CAPACITY; i++) {
        if (s_entries[i].pipeline) s_entries[i].pipeline->Release();
    }
    if (s_uber_pipeline) s_uber_pipeline->Release();
    if (s_library) s_library->Release();
    s_uber_pipeline = nullptr;
    s_library = nullptr;
}

bool ged3d12_pipeline_cache_init(void) {
    if (s_ready || s_lock_initialized) {
        fprintf(stderr, "ged3d12: pipeline cache already initialized\n");
        return false;
    }
    if (!ged3d12_device() || !ged3d12_root_signature()) {
        fprintf(stderr,
                "ged3d12: pipeline cache requires an initialized device\n");
        return false;
    }

    memset(s_entries, 0, sizeof(s_entries));
    memset(s_queue, 0, sizeof(s_queue));
    memset(&s_stats, 0, sizeof(s_stats));
    s_queue_read = 0;
    s_queue_write = 0;
    s_queue_count = 0;
    s_stopping = 0;
    s_library_dirty = 0;
    s_log = env_enabled("SR_D3D12_PSO_LOG");
    s_cache_path[0] = '\0';
    configure_cache_path();
    initialize_pipeline_library();

    ged3d12_pipeline_uber_key(&s_uber_key);
    PipelineBuildResult uber = load_or_create_pipeline(&s_uber_key);
    s_uber_pipeline = uber.pipeline;
    if (!s_uber_pipeline) {
        fprintf(stderr, "ged3d12: create ubershader fallback PSO failed\n");
        release_pipeline_objects();
        memset(&s_stats, 0, sizeof(s_stats));
        s_log = 0;
        return false;
    }
    if (uber.library_hit) s_stats.library_hits++;
    if (uber.compiled) s_stats.compiled++;
    if (uber.cache_write) s_stats.cache_writes++;

    InitializeCriticalSection(&s_lock);
    s_lock_initialized = 1;
    s_worker_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!s_worker_event) {
        fprintf(stderr,
                "ged3d12: create PSO worker event failed (error=%lu)\n",
                (unsigned long)GetLastError());
        DeleteCriticalSection(&s_lock);
        s_lock_initialized = 0;
        release_pipeline_objects();
        memset(&s_stats, 0, sizeof(s_stats));
        s_log = 0;
        return false;
    }
    s_worker = CreateThread(
        nullptr, 0, pipeline_worker, nullptr, 0, nullptr);
    if (!s_worker) {
        fprintf(stderr,
                "ged3d12: create PSO worker thread failed (error=%lu)\n",
                (unsigned long)GetLastError());
        CloseHandle(s_worker_event);
        s_worker_event = nullptr;
        DeleteCriticalSection(&s_lock);
        s_lock_initialized = 0;
        release_pipeline_objects();
        memset(&s_stats, 0, sizeof(s_stats));
        s_log = 0;
        return false;
    }

    s_ready = 1;
    GeD3D12PipelineKey cull_key = s_uber_key;
    cull_key.cull_mode = D3D12_CULL_MODE_BACK;
    uint64_t cull_hash = ged3d12_pipeline_key_hash(&cull_key);
    UINT cull_index = UINT_MAX;
    EnterCriticalSection(&s_lock);
    for (UINT probe = 0; probe < PIPELINE_CACHE_CAPACITY; probe++) {
        UINT index =
            (UINT)(cull_hash + probe) & (PIPELINE_CACHE_CAPACITY - 1);
        if (s_entries[index].state == PIPELINE_ENTRY_FREE) {
            cull_index = index;
            break;
        }
    }
    bool queued =
        queue_pipeline_locked(
            &cull_key, cull_hash, cull_index, false);
    LeaveCriticalSection(&s_lock);
    if (!queued) {
        fprintf(stderr, "ged3d12: no slot for cull PSO prewarm\n");
        EnterCriticalSection(&s_lock);
        s_stats.failures++;
        LeaveCriticalSection(&s_lock);
    } else if (!SetEvent(s_worker_event)) {
        fprintf(stderr,
                "ged3d12: queue cull PSO prewarm failed (error=%lu)\n",
                (unsigned long)GetLastError());
        EnterCriticalSection(&s_lock);
        s_stats.failures++;
        LeaveCriticalSection(&s_lock);
    }
    if (s_log) {
        fprintf(
            stderr,
            "ged3d12: ubershader fallback PSO source=%s; "
            "async compiler ready, cull PSO prewarmed\n",
            uber.library_hit ? "library" : "compiler");
    }
    return true;
}

void ged3d12_pipeline_cache_shutdown(void) {
    if (s_lock_initialized) {
        EnterCriticalSection(&s_lock);
        s_stopping = 1;
        LeaveCriticalSection(&s_lock);
        if (s_worker_event) SetEvent(s_worker_event);
        if (s_worker) {
            DWORD wait = WaitForSingleObject(s_worker, INFINITE);
            if (wait != WAIT_OBJECT_0) {
                fprintf(stderr,
                        "ged3d12: join PSO worker failed "
                        "(wait=%lu, error=%lu)\n",
                        (unsigned long)wait,
                        (unsigned long)GetLastError());
            }
            CloseHandle(s_worker);
        }
        if (s_worker_event) CloseHandle(s_worker_event);
        s_worker = nullptr;
        s_worker_event = nullptr;
    }

    if (save_pipeline_library()) s_stats.cache_writes++;
    if (s_log && s_ready) {
        fprintf(
            stderr,
            "ged3d12: PSO cache entries=%u pending=%u "
            "lookups=%llu hits=%llu misses=%llu fallback=%llu "
            "compiled=%u library_hits=%u writes=%u failures=%u\n",
            s_stats.entries, s_stats.pending,
            (unsigned long long)s_stats.lookups,
            (unsigned long long)s_stats.hits,
            (unsigned long long)s_stats.misses,
            (unsigned long long)s_stats.fallback_binds,
            s_stats.compiled, s_stats.library_hits,
            s_stats.cache_writes, s_stats.failures);
    }
    release_pipeline_objects();
    if (s_lock_initialized) {
        DeleteCriticalSection(&s_lock);
        s_lock_initialized = 0;
    }
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_queue, 0, sizeof(s_queue));
    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_uber_key, 0, sizeof(s_uber_key));
    s_queue_read = 0;
    s_queue_write = 0;
    s_queue_count = 0;
    s_stopping = 0;
    s_library_dirty = 0;
    s_ready = 0;
    s_log = 0;
    s_cache_path[0] = '\0';
}

bool ged3d12_pipeline_bind(const GeD3D12PipelineKey *key) {
    if (!s_ready || !key) {
        fprintf(stderr, "ged3d12: invalid pipeline cache lookup\n");
        return false;
    }
    if (!ged3d12_pipeline_uber_compatible(key)) {
        fprintf(stderr,
                "ged3d12: PSO key is not ubershader-compatible "
                "(hash=%016llx)\n",
                (unsigned long long)ged3d12_pipeline_key_hash(key));
        EnterCriticalSection(&s_lock);
        s_stats.failures++;
        LeaveCriticalSection(&s_lock);
        return false;
    }

    ID3D12PipelineState *pipeline = s_uber_pipeline;
    bool wake_worker = false;
    EnterCriticalSection(&s_lock);
    s_stats.lookups++;
    if (memcmp(key, &s_uber_key, sizeof(*key)) == 0) {
        s_stats.fallback_binds++;
    } else {
        uint64_t hash = ged3d12_pipeline_key_hash(key);
        UINT first_free = UINT_MAX;
        PipelineEntry *found = nullptr;
        for (UINT probe = 0; probe < PIPELINE_CACHE_CAPACITY; probe++) {
            UINT index =
                (UINT)(hash + probe) & (PIPELINE_CACHE_CAPACITY - 1);
            PipelineEntry *entry = &s_entries[index];
            if (entry->state == PIPELINE_ENTRY_FREE) {
                first_free = index;
                break;
            }
            if (entry->hash == hash &&
                memcmp(&entry->key, key, sizeof(*key)) == 0) {
                found = entry;
                break;
            }
        }
        if (found && found->state == PIPELINE_ENTRY_READY) {
            pipeline = found->pipeline;
            s_stats.hits++;
        } else if (found) {
            s_stats.fallback_binds++;
        } else if (queue_pipeline_locked(
                       key, hash, first_free, true)) {
            s_stats.fallback_binds++;
            wake_worker = true;
        } else {
            fprintf(stderr,
                    "ged3d12: GE pipeline cache exhausted "
                    "(%u entries); using ubershader fallback\n",
                    PIPELINE_CACHE_CAPACITY);
            s_stats.failures++;
            s_stats.fallback_binds++;
        }
    }
    LeaveCriticalSection(&s_lock);

    if (wake_worker && !SetEvent(s_worker_event)) {
        fprintf(stderr,
                "ged3d12: wake PSO worker failed (error=%lu)\n",
                (unsigned long)GetLastError());
        EnterCriticalSection(&s_lock);
        s_stats.failures++;
        LeaveCriticalSection(&s_lock);
    }
    ged3d12_command_list()->SetPipelineState(pipeline);
    return true;
}

GeD3D12PipelineStats ged3d12_pipeline_stats(void) {
    if (!s_lock_initialized) return s_stats;
    EnterCriticalSection(&s_lock);
    GeD3D12PipelineStats stats = s_stats;
    LeaveCriticalSection(&s_lock);
    return stats;
}
