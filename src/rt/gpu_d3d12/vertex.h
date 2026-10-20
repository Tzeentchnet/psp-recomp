/* CPU-decoded GE vertex capture and fenced upload-ring staging. */
#ifndef GE_D3D12_VERTEX_H
#define GE_D3D12_VERTEX_H

#include "../ge_shared.h"
#include "pipeline.h"

#include <d3d12.h>
#include <stddef.h>
#include <stdint.h>

struct GeD3D12Vertex {
    float x, y, z, rw;
    float u, v;
    float fog;
    uint32_t rgba;
};

enum GeD3D12Primitive : uint8_t {
    GE_D3D12_PRIMITIVE_TRIANGLE = 0,
    GE_D3D12_PRIMITIVE_SPRITE = 1,
    GE_D3D12_PRIMITIVE_LINE = 2,
    GE_D3D12_PRIMITIVE_POINT = 3,
    GE_D3D12_PRIMITIVE_COUNT = 4,
};

struct GeD3D12VertexBatch {
    uint32_t first;
    uint32_t count;
    uint8_t primitive;
    uint8_t perspective;
    uint16_t reserved;
    GeD3D12PipelineKey pipeline;
};

struct GeD3D12VertexFrame {
    D3D12_VERTEX_BUFFER_VIEW buffer;
    const GeD3D12VertexBatch *batches;
    uint32_t vertex_count;
    uint32_t batch_count;
    uint32_t primitive_count[GE_D3D12_PRIMITIVE_COUNT];
    uint64_t hash;
};

bool ged3d12_vertex_capture_init(void);
void ged3d12_vertex_capture_shutdown(void);

/* These hooks deliberately return zero until native PSOs replace the software rasterizer. */
int ged3d12_vertex_capture_tri(
    const GeVtx *a, const GeVtx *b, const GeVtx *c, int perspective);
int ged3d12_vertex_capture_sprite(
    const GeVtx *p0, const GeVtx *p1, int perspective);
int ged3d12_vertex_capture_line(
    const GeVtx *a, const GeVtx *b, int perspective);
int ged3d12_vertex_capture_point(const GeVtx *a, int perspective);

bool ged3d12_vertex_upload_pending(GeD3D12VertexFrame *out);

static_assert(sizeof(GeD3D12Vertex) == 32, "D3D12 GE vertex ABI must remain 32 bytes");
static_assert(offsetof(GeD3D12Vertex, rw) == 12, "D3D12 GE position ABI changed");
static_assert(offsetof(GeD3D12Vertex, u) == 16, "D3D12 GE texture ABI changed");
static_assert(offsetof(GeD3D12Vertex, fog) == 24, "D3D12 GE fog ABI changed");
static_assert(offsetof(GeD3D12Vertex, rgba) == 28, "D3D12 GE color ABI changed");
static_assert(sizeof(GeD3D12VertexBatch) == 28, "D3D12 GE batch ABI changed");

#endif
