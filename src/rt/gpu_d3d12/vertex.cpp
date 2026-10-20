#include "vertex.h"

#include "device.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_VERTEX_CAPACITY 4096u
#define INITIAL_BATCH_CAPACITY 2048u
/* Leaves over 24 MiB in each frame partition for textures and presentation. */
#define MAX_FRAME_VERTICES 240000u
#define MAX_FRAME_BATCHES (MAX_FRAME_VERTICES / 3u)

static GeD3D12Vertex *s_vertices;
static GeD3D12VertexBatch *s_batches;
static uint32_t s_vertex_count;
static uint32_t s_vertex_capacity;
static uint32_t s_batch_count;
static uint32_t s_batch_capacity;
static uint32_t s_primitive_count[GE_D3D12_PRIMITIVE_COUNT];
static int s_capture_failed;

static float snap16(float value) {
    return floorf(value * 16.0f + 0.375f) * (1.0f / 16.0f);
}

static void capture_error(const char *message, uint32_t requested, uint32_t limit) {
    if (!s_capture_failed) {
        fprintf(stderr, "ged3d12: %s (%u requested, %u limit)\n",
                message, requested, limit);
    }
    s_capture_failed = 1;
}

static bool reserve_vertices(uint32_t additional) {
    if (!s_vertices || additional > MAX_FRAME_VERTICES - s_vertex_count) {
        capture_error("decoded vertex frame capacity exceeded",
                      s_vertex_count + additional, MAX_FRAME_VERTICES);
        return false;
    }
    uint32_t required = s_vertex_count + additional;
    if (required <= s_vertex_capacity) return true;
    uint32_t capacity = s_vertex_capacity;
    while (capacity < required) {
        capacity = capacity > MAX_FRAME_VERTICES / 2u
            ? MAX_FRAME_VERTICES : capacity * 2u;
    }
    void *memory = realloc(s_vertices, (size_t)capacity * sizeof(*s_vertices));
    if (!memory) {
        capture_error("could not grow decoded vertex storage", required, capacity);
        return false;
    }
    s_vertices = static_cast<GeD3D12Vertex *>(memory);
    s_vertex_capacity = capacity;
    return true;
}

static bool reserve_batch(void) {
    if (!s_batches || s_batch_count >= MAX_FRAME_BATCHES) {
        capture_error("decoded vertex batch capacity exceeded",
                      s_batch_count + 1u, MAX_FRAME_BATCHES);
        return false;
    }
    if (s_batch_count < s_batch_capacity) return true;
    uint32_t capacity = s_batch_capacity > MAX_FRAME_BATCHES / 2u
        ? MAX_FRAME_BATCHES : s_batch_capacity * 2u;
    void *memory = realloc(s_batches, (size_t)capacity * sizeof(*s_batches));
    if (!memory) {
        capture_error("could not grow decoded vertex batch storage",
                      s_batch_count + 1u, capacity);
        return false;
    }
    s_batches = static_cast<GeD3D12VertexBatch *>(memory);
    s_batch_capacity = capacity;
    return true;
}

static bool begin_primitive(
    GeD3D12Primitive primitive, int perspective, uint32_t vertex_count,
    uint32_t *first) {
    if (s_capture_failed || !reserve_vertices(vertex_count)) {
        return false;
    }
    *first = s_vertex_count;
    GeDebugContext debug = ge_debug_context();
    if (s_batch_count) {
        GeD3D12VertexBatch *last = &s_batches[s_batch_count - 1u];
        if (last->first + last->count == s_vertex_count &&
            last->primitive == primitive &&
            last->perspective == (uint8_t)(perspective != 0) &&
            last->list_id == debug.list_id &&
            last->list_addr == debug.list_addr &&
            last->draw_serial == debug.draw_serial &&
            last->draw_pc == debug.draw_pc &&
            last->draw_op == debug.draw_op) {
            last->count += vertex_count;
            s_primitive_count[primitive]++;
            return true;
        }
    }
    if (!reserve_batch()) return false;
    GeD3D12VertexBatch *batch = &s_batches[s_batch_count];
    batch->first = *first;
    batch->count = vertex_count;
    batch->primitive = primitive;
    batch->perspective = perspective != 0;
    batch->reserved = 0;
    batch->list_id = debug.list_id;
    batch->list_addr = debug.list_addr;
    batch->draw_serial = debug.draw_serial;
    batch->draw_pc = debug.draw_pc;
    batch->draw_op = debug.draw_op;
    ged3d12_pipeline_key_build(
        ge_state_ptr(), primitive != GE_D3D12_PRIMITIVE_TRIANGLE,
        &batch->pipeline);
    if (!ged3d12_framebuffer_capture(&batch->framebuffer)) {
        capture_error(
            "framebuffer capture failed", s_batch_count + 1u,
            MAX_FRAME_BATCHES);
        return false;
    }
    if (!ged3d12_texture_capture(&batch->texture)) {
        capture_error(
            "texture capture failed", s_batch_count + 1u, MAX_FRAME_BATCHES);
        return false;
    }
    ged3d12_shader_constants_build(
        ge_state_ptr(), perspective,
        primitive == GE_D3D12_PRIMITIVE_SPRITE,
        primitive == GE_D3D12_PRIMITIVE_TRIANGLE,
        &batch->texture, &batch->shader);
    s_batch_count++;
    s_primitive_count[primitive]++;
    return true;
}

static void put_vertex(
    float x, float y, float z, float rw, float u, float v, float fog,
    int red, int green, int blue, int alpha) {
    GeD3D12Vertex *vertex = &s_vertices[s_vertex_count++];
    vertex->x = snap16(x);
    vertex->y = snap16(y);
    vertex->z = z;
    vertex->rw = rw;
    vertex->u = u;
    vertex->v = v;
    vertex->fog = fog;
    uint32_t r = (uint32_t)(red < 0 ? 0 : red > 255 ? 255 : red);
    uint32_t g = (uint32_t)(green < 0 ? 0 : green > 255 ? 255 : green);
    uint32_t b = (uint32_t)(blue < 0 ? 0 : blue > 255 ? 255 : blue);
    uint32_t a = (uint32_t)(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha);
    vertex->rgba = r | (g << 8) | (b << 16) | (a << 24);
}

bool ged3d12_vertex_capture_init(void) {
    if (s_vertices || s_batches) {
        fprintf(stderr, "ged3d12: decoded vertex capture already initialized\n");
        return false;
    }
    s_vertices = static_cast<GeD3D12Vertex *>(
        malloc(INITIAL_VERTEX_CAPACITY * sizeof(*s_vertices)));
    s_batches = static_cast<GeD3D12VertexBatch *>(
        malloc(INITIAL_BATCH_CAPACITY * sizeof(*s_batches)));
    if (!s_vertices || !s_batches) {
        fprintf(stderr, "ged3d12: could not allocate decoded vertex capture storage\n");
        ged3d12_vertex_capture_shutdown();
        return false;
    }
    s_vertex_capacity = INITIAL_VERTEX_CAPACITY;
    s_batch_capacity = INITIAL_BATCH_CAPACITY;
    s_vertex_count = 0;
    s_batch_count = 0;
    s_capture_failed = 0;
    memset(s_primitive_count, 0, sizeof(s_primitive_count));
    return true;
}

void ged3d12_vertex_capture_shutdown(void) {
    free(s_vertices);
    free(s_batches);
    s_vertices = nullptr;
    s_batches = nullptr;
    s_vertex_count = 0;
    s_vertex_capacity = 0;
    s_batch_count = 0;
    s_batch_capacity = 0;
    s_capture_failed = 0;
    memset(s_primitive_count, 0, sizeof(s_primitive_count));
}

int ged3d12_vertex_capture_tri(
    const GeVtx *a, const GeVtx *b, const GeVtx *c, int perspective) {
    uint32_t first;
    if (!begin_primitive(
            GE_D3D12_PRIMITIVE_TRIANGLE, perspective, 3, &first)) {
        return 0;
    }
    (void)first;
    const GeState *state = ge_state_ptr();
    int flat = !state->shade_gouraud || state->clear;
    const GeVtx *color_a = flat ? c : a;
    const GeVtx *color_b = flat ? c : b;
    put_vertex(a->x, a->y, a->z, a->rw, a->u, a->v, a->fog,
               color_a->r, color_a->g, color_a->b, color_a->a);
    put_vertex(b->x, b->y, b->z, b->rw, b->u, b->v, b->fog,
               color_b->r, color_b->g, color_b->b, color_b->a);
    put_vertex(c->x, c->y, c->z, c->rw, c->u, c->v, c->fog,
               c->r, c->g, c->b, c->a);
    return 0;
}

int ged3d12_vertex_capture_sprite(
    const GeVtx *p0, const GeVtx *p1, int perspective) {
    float xa = floorf(fminf(p0->x, p1->x));
    float xb = floorf(fmaxf(p0->x, p1->x));
    float ya = floorf(fminf(p0->y, p1->y));
    float yb = floorf(fmaxf(p0->y, p1->y));
    if (xb <= xa || yb <= ya) return 0;

    uint32_t first;
    if (!begin_primitive(
            GE_D3D12_PRIMITIVE_SPRITE, perspective, 6, &first)) {
        return 0;
    }
    (void)first;

    float u0 = p0->u, v0 = p0->v, u1 = p1->u, v1 = p1->v;
    if (perspective) {
        u0 = p0->u / (p0->rw != 0.0f ? p0->rw : 1.0f);
        v0 = p0->v / (p0->rw != 0.0f ? p0->rw : 1.0f);
        u1 = p1->u / (p1->rw != 0.0f ? p1->rw : 1.0f);
        v1 = p1->v / (p1->rw != 0.0f ? p1->rw : 1.0f);
    }
    float texel_width = (u1 - u0) / (xb - xa);
    float texel_height = (v1 - v0) / (yb - ya);
    float ua = u0 - 0.5f * texel_width;
    float ub = u1 - 0.5f * texel_width;
    float va = v0 - 0.5f * texel_height;
    float vb = v1 - 0.5f * texel_height;

    float z = p1->z;
    float fog = p1->fog;
    put_vertex(xa, ya, z, 1.0f, ua, va, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xb, ya, z, 1.0f, ub, va, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xa, yb, z, 1.0f, ua, vb, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xb, ya, z, 1.0f, ub, va, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xb, yb, z, 1.0f, ub, vb, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xa, yb, z, 1.0f, ua, vb, fog, p1->r, p1->g, p1->b, p1->a);
    return 0;
}

int ged3d12_vertex_capture_line(
    const GeVtx *a, const GeVtx *b, int perspective) {
    uint32_t first;
    if (!begin_primitive(
            GE_D3D12_PRIMITIVE_LINE, perspective, 6, &first)) {
        return 0;
    }
    (void)first;
    float dx = b->x - a->x;
    float dy = b->y - a->y;
    float length = sqrtf(dx * dx + dy * dy);
    if (length < 1e-6f) {
        dx = 1.0f;
        dy = 0.0f;
        length = 1.0f;
    }
    float nx = -dy / length * 0.5f;
    float ny = dx / length * 0.5f;
    float ex = dx / length * 0.5f;
    float ey = dy / length * 0.5f;
    float ax = a->x + 0.5f;
    float ay = a->y + 0.5f;
    float bx = b->x + 0.5f;
    float by = b->y + 0.5f;

    const GeState *state = ge_state_ptr();
    const GeVtx *color_a = state->shade_gouraud ? a : b;
    put_vertex(ax - ex - nx, ay - ey - ny, a->z, a->rw, a->u, a->v, a->fog,
               color_a->r, color_a->g, color_a->b, color_a->a);
    put_vertex(ax - ex + nx, ay - ey + ny, a->z, a->rw, a->u, a->v, a->fog,
               color_a->r, color_a->g, color_a->b, color_a->a);
    put_vertex(bx + ex - nx, by + ey - ny, b->z, b->rw, b->u, b->v, b->fog,
               b->r, b->g, b->b, b->a);
    put_vertex(ax - ex + nx, ay - ey + ny, a->z, a->rw, a->u, a->v, a->fog,
               color_a->r, color_a->g, color_a->b, color_a->a);
    put_vertex(bx + ex + nx, by + ey + ny, b->z, b->rw, b->u, b->v, b->fog,
               b->r, b->g, b->b, b->a);
    put_vertex(bx + ex - nx, by + ey - ny, b->z, b->rw, b->u, b->v, b->fog,
               b->r, b->g, b->b, b->a);
    return 0;
}

int ged3d12_vertex_capture_point(const GeVtx *a, int perspective) {
    uint32_t first;
    if (!begin_primitive(
            GE_D3D12_PRIMITIVE_POINT, perspective, 6, &first)) {
        return 0;
    }
    (void)first;
    float x0 = floorf(a->x);
    float y0 = floorf(a->y);
    put_vertex(x0, y0, a->z, a->rw, a->u, a->v, a->fog, a->r, a->g, a->b, a->a);
    put_vertex(x0 + 1.0f, y0, a->z, a->rw, a->u, a->v, a->fog, a->r, a->g, a->b, a->a);
    put_vertex(x0, y0 + 1.0f, a->z, a->rw, a->u, a->v, a->fog, a->r, a->g, a->b, a->a);
    put_vertex(x0 + 1.0f, y0, a->z, a->rw, a->u, a->v, a->fog, a->r, a->g, a->b, a->a);
    put_vertex(x0 + 1.0f, y0 + 1.0f, a->z, a->rw, a->u, a->v, a->fog, a->r, a->g, a->b, a->a);
    put_vertex(x0, y0 + 1.0f, a->z, a->rw, a->u, a->v, a->fog, a->r, a->g, a->b, a->a);
    return 0;
}

static uint64_t hash_vertices(const GeD3D12Vertex *vertices, uint32_t count) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(vertices);
    size_t size = (size_t)count * sizeof(*vertices);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool ged3d12_vertex_upload_pending(GeD3D12VertexFrame *out) {
    if (!out) {
        fprintf(stderr, "ged3d12: decoded vertex upload requires an output frame\n");
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (s_capture_failed) {
        fprintf(stderr, "ged3d12: decoded vertex capture failed; refusing a partial upload\n");
        return false;
    }
    if (!s_vertex_count) {
        s_batch_count = 0;
        memset(s_primitive_count, 0, sizeof(s_primitive_count));
        return true;
    }

    UINT64 size = (UINT64)s_vertex_count * sizeof(*s_vertices);
    GeD3D12UploadSlice upload = {};
    if (!ged3d12_upload_alloc(size, 16, &upload)) {
        fprintf(stderr, "ged3d12: could not allocate %llu bytes for decoded vertices\n",
                (unsigned long long)size);
        return false;
    }
    memcpy(upload.cpu, s_vertices, (size_t)size);

    out->buffer.BufferLocation = upload.gpu;
    out->buffer.SizeInBytes = (UINT)size;
    out->buffer.StrideInBytes = sizeof(*s_vertices);
    out->batches = s_batches;
    out->vertex_count = s_vertex_count;
    out->batch_count = s_batch_count;
    memcpy(out->primitive_count, s_primitive_count, sizeof(out->primitive_count));
    out->hash = hash_vertices(s_vertices, s_vertex_count);

    s_vertex_count = 0;
    s_batch_count = 0;
    memset(s_primitive_count, 0, sizeof(s_primitive_count));
    return true;
}

void ged3d12_vertex_pending_counts(
    uint32_t *vertex_count, uint32_t *batch_count) {
    if (vertex_count) *vertex_count = s_vertex_count;
    if (batch_count) *batch_count = s_batch_count;
}
