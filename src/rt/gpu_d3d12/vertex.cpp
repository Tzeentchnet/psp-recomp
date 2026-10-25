#include "vertex.h"

#include "device.h"
#include "../perf_stats.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_VERTEX_CAPACITY 4096u
#define INITIAL_BATCH_CAPACITY 2048u
/* Bound malformed display lists while allowing dense frames to spill into the
 * upload allocator's fenced overflow slabs. */
#define MAX_FRAME_VERTICES (1024u * 1024u)
#define MAX_FRAME_BATCHES (256u * 1024u)

static GeD3D12Vertex *s_vertices;
static GeD3D12VertexBatch *s_batches;
static uint16_t *s_indices;
static uint32_t s_index_count;
static uint32_t s_index_capacity;
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

/* Pipeline key and shader constants are pure functions of the GE state (plus the captured
 * texture binding). Consecutive draws very often share all of it, so reuse the previous result
 * when the whole state (ignoring the vertex/index pointers) and the binding are unchanged.
 * SR_D3D12_VERIFY_STATE_MEMO=1 rebuilds and compares on every reuse. */
static GeState s_memo_state;
static GeD3D12PipelineKey s_memo_pipeline;
static GeD3D12ShaderConstants s_memo_shader;
static GeD3D12TextureBinding s_memo_texture;
static int s_memo_primitive = -1;
static int s_memo_perspective;
static int s_memo_verify = -1;

static void build_pipeline_and_shader(
    GeD3D12Primitive primitive, int perspective, GeD3D12VertexBatch *batch) {
    const GeState *state = ge_state_ptr();
    if (s_memo_verify < 0) s_memo_verify = getenv("SR_D3D12_VERIFY_STATE_MEMO") ? 1 : 0;
    bool reuse = false;
    if (s_memo_primitive == (int)primitive && s_memo_perspective == perspective) {
        s_memo_state.vaddr = state->vaddr;
        s_memo_state.iaddr = state->iaddr;
        reuse = memcmp(&s_memo_state, state, sizeof(*state)) == 0 &&
                memcmp(&s_memo_texture, &batch->texture, sizeof(batch->texture)) == 0;
    }
    if (reuse && !s_memo_verify) {
        batch->pipeline = s_memo_pipeline;
        batch->shader = s_memo_shader;
        return;
    }
    ged3d12_pipeline_key_build(
        state, primitive != GE_D3D12_PRIMITIVE_TRIANGLE, &batch->pipeline);
    ged3d12_shader_constants_build(
        state, perspective,
        primitive == GE_D3D12_PRIMITIVE_SPRITE,
        primitive == GE_D3D12_PRIMITIVE_TRIANGLE,
        &batch->texture, &batch->shader);
    if (reuse) {
        if (memcmp(&batch->pipeline, &s_memo_pipeline, sizeof(batch->pipeline)) != 0 ||
            memcmp(&batch->shader, &s_memo_shader, sizeof(batch->shader)) != 0) {
            fprintf(stderr, "ged3d12: state memo mismatch (primitive=%d)\n", (int)primitive);
            abort();
        }
        return;
    }
    memcpy(&s_memo_state, state, sizeof(*state));
    s_memo_pipeline = batch->pipeline;
    s_memo_shader = batch->shader;
    s_memo_texture = batch->texture;
    s_memo_primitive = (int)primitive;
    s_memo_perspective = perspective;
}

static bool begin_primitive_ex(
    GeD3D12Primitive primitive, int perspective, uint32_t vertex_count,
    uint32_t *first, bool allow_merge) {
    if (s_capture_failed || !reserve_vertices(vertex_count)) {
        return false;
    }
    *first = s_vertex_count;
    GeDebugContext debug = ge_debug_context();
    if (s_batch_count && allow_merge) {
        GeD3D12VertexBatch *last = &s_batches[s_batch_count - 1u];
        if (!(last->flags & GE_D3D12_BATCH_INDEXED) &&
            last->first + last->count == s_vertex_count &&
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
    batch->flags = 0;
    batch->index_first = 0;
    batch->table_count = 0;
    batch->list_id = debug.list_id;
    batch->list_addr = debug.list_addr;
    batch->draw_serial = debug.draw_serial;
    batch->draw_pc = debug.draw_pc;
    batch->draw_op = debug.draw_op;
    SrPerfCategory perf_previous = sr_perf_switch(SR_PERF_STATE_CAPTURE);
    if (!ged3d12_framebuffer_capture(&batch->framebuffer)) {
        sr_perf_switch(perf_previous);
        capture_error(
            "framebuffer capture failed", s_batch_count + 1u,
            MAX_FRAME_BATCHES);
        return false;
    }
    sr_perf_switch(SR_PERF_TEXTURE_CAPTURE);
    if (!ged3d12_texture_capture(&batch->texture)) {
        sr_perf_switch(perf_previous);
        capture_error(
            "texture capture failed", s_batch_count + 1u, MAX_FRAME_BATCHES);
        return false;
    }
    sr_perf_switch(SR_PERF_STATE_CAPTURE);
    build_pipeline_and_shader(primitive, perspective, batch);
    sr_perf_switch(perf_previous);
    s_batch_count++;
    s_primitive_count[primitive]++;
    return true;
}

static bool begin_primitive(
    GeD3D12Primitive primitive, int perspective, uint32_t vertex_count,
    uint32_t *first) {
    return begin_primitive_ex(primitive, perspective, vertex_count, first, true);
}

static bool reserve_indices(uint32_t additional) {
    if (!s_indices || additional > MAX_FRAME_VERTICES * 4u - s_index_count) {
        capture_error("index frame capacity exceeded",
                      s_index_count + additional, MAX_FRAME_VERTICES * 4u);
        return false;
    }
    uint32_t required = s_index_count + additional;
    if (required <= s_index_capacity) return true;
    uint32_t capacity = s_index_capacity;
    while (capacity < required) capacity *= 2u;
    void *memory = realloc(s_indices, (size_t)capacity * sizeof(*s_indices));
    if (!memory) {
        capture_error("could not grow index storage", required, capacity);
        return false;
    }
    s_indices = static_cast<uint16_t *>(memory);
    s_index_capacity = capacity;
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

static void feedback_axis_bounds(
    float minimum, float maximum, uint32_t size, uint32_t offset,
    bool clamp_value, bool nearest,
    uint16_t *first_out, uint16_t *end_out) {
    int first;
    int end;
    if (!size || !isfinite(minimum) || !isfinite(maximum)) {
        *first_out = (uint16_t)offset;
        *end_out = (uint16_t)(
            offset + size > GE_D3D12_FRAMEBUFFER_WIDTH
                ? GE_D3D12_FRAMEBUFFER_WIDTH : offset + size);
        return;
    }
    if (minimum > maximum) {
        float swap = minimum;
        minimum = maximum;
        maximum = swap;
    }
    if (nearest) {
        first = (int)floorf(minimum + 0.5f);
        end = (int)floorf(maximum + 0.5f) + 1;
    } else {
        first = (int)floorf(minimum - 0.5f);
        end = (int)floorf(maximum - 0.5f) + 2;
    }
    if (!clamp_value && (first < 0 || end > (int)size)) {
        first = 0;
        end = (int)size;
    } else {
        if (first < 0) first = 0;
        if (first >= (int)size) first = (int)size - 1;
        if (end <= first) end = first + 1;
        if (end > (int)size) end = (int)size;
    }
    first += (int)offset;
    end += (int)offset;
    if (first < 0) first = 0;
    if (end > (int)GE_D3D12_FRAMEBUFFER_WIDTH)
        end = GE_D3D12_FRAMEBUFFER_WIDTH;
    if (first >= end) {
        first = 0;
        end = GE_D3D12_FRAMEBUFFER_WIDTH;
    }
    *first_out = (uint16_t)first;
    *end_out = (uint16_t)end;
}

void ged3d12_vertex_feedback_bounds(
    GeD3D12TextureBinding *texture,
    const GeD3D12Vertex *vertices, uint32_t count) {
    if (!texture || !vertices || !count ||
        texture->framebuffer.handle ==
            GE_D3D12_FRAMEBUFFER_HANDLE_NONE) {
        return;
    }
    float minimum_u = INFINITY;
    float minimum_v = INFINITY;
    float maximum_u = -INFINITY;
    float maximum_v = -INFINITY;
    for (uint32_t i = 0; i < count; i++) {
        float rw = fmaxf(fabsf(vertices[i].rw), 1e-20f);
        float u = vertices[i].u / rw;
        float v = vertices[i].v / rw;
        minimum_u = fminf(minimum_u, u);
        minimum_v = fminf(minimum_v, v);
        maximum_u = fmaxf(maximum_u, u);
        maximum_v = fmaxf(maximum_v, v);
    }
    bool nearest =
        !texture->sampler.min_linear &&
        !texture->sampler.mag_linear;
    GeD3D12FramebufferAlias *alias = &texture->framebuffer;
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
    feedback_axis_bounds(
        minimum_u, maximum_u, texture->width, alias->x,
        texture->sampler.clamp_u != 0, nearest, &left, &right);
    feedback_axis_bounds(
        minimum_v, maximum_v, texture->height, alias->y,
        texture->sampler.clamp_v != 0, nearest, &top, &bottom);
    if (alias->snapshot_right > alias->snapshot_left &&
        alias->snapshot_bottom > alias->snapshot_top) {
        if (left < alias->snapshot_left) alias->snapshot_left = left;
        if (top < alias->snapshot_top) alias->snapshot_top = top;
        if (right > alias->snapshot_right) alias->snapshot_right = right;
        if (bottom > alias->snapshot_bottom) alias->snapshot_bottom = bottom;
    } else {
        alias->snapshot_left = left;
        alias->snapshot_top = top;
        alias->snapshot_right = right;
        alias->snapshot_bottom = bottom;
    }
}

void ged3d12_vertex_draw_bounds(
    GeD3D12FramebufferBinding *framebuffer,
    const GeD3D12Vertex *vertices, uint32_t count) {
    if (!framebuffer || !vertices || !count) return;
    float minimum_x = INFINITY;
    float minimum_y = INFINITY;
    float maximum_x = -INFINITY;
    float maximum_y = -INFINITY;
    for (uint32_t i = 0; i < count; i++) {
        minimum_x = fminf(minimum_x, vertices[i].x);
        minimum_y = fminf(minimum_y, vertices[i].y);
        maximum_x = fmaxf(maximum_x, vertices[i].x);
        maximum_y = fmaxf(maximum_y, vertices[i].y);
    }
    int left = isfinite(minimum_x) ? (int)floorf(minimum_x) : 0;
    int top = isfinite(minimum_y) ? (int)floorf(minimum_y) : 0;
    int right = isfinite(maximum_x)
        ? (int)ceilf(maximum_x) : GE_D3D12_FRAMEBUFFER_WIDTH;
    int bottom = isfinite(maximum_y)
        ? (int)ceilf(maximum_y) : GE_D3D12_FRAMEBUFFER_HEIGHT;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int)GE_D3D12_FRAMEBUFFER_WIDTH)
        right = GE_D3D12_FRAMEBUFFER_WIDTH;
    if (bottom > (int)GE_D3D12_FRAMEBUFFER_HEIGHT)
        bottom = GE_D3D12_FRAMEBUFFER_HEIGHT;
    if (right <= left || bottom <= top) {
        left = 0;
        top = 0;
        right = GE_D3D12_FRAMEBUFFER_WIDTH;
        bottom = GE_D3D12_FRAMEBUFFER_HEIGHT;
    }
    if (framebuffer->draw_right > framebuffer->draw_left &&
        framebuffer->draw_bottom > framebuffer->draw_top) {
        if (left < framebuffer->draw_left)
            framebuffer->draw_left = (uint16_t)left;
        if (top < framebuffer->draw_top)
            framebuffer->draw_top = (uint16_t)top;
        if (right > framebuffer->draw_right)
            framebuffer->draw_right = (uint16_t)right;
        if (bottom > framebuffer->draw_bottom)
            framebuffer->draw_bottom = (uint16_t)bottom;
    } else {
        framebuffer->draw_left = (uint16_t)left;
        framebuffer->draw_top = (uint16_t)top;
        framebuffer->draw_right = (uint16_t)right;
        framebuffer->draw_bottom = (uint16_t)bottom;
    }
}

static void update_batch_bounds(uint32_t first, uint32_t count) {
    if (!s_batch_count || first > s_vertex_count ||
        count > s_vertex_count - first) {
        return;
    }
    ged3d12_vertex_draw_bounds(
        &s_batches[s_batch_count - 1u].framebuffer,
        &s_vertices[first], count);
    ged3d12_vertex_feedback_bounds(
        &s_batches[s_batch_count - 1u].texture,
        &s_vertices[first], count);
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
    s_indices = static_cast<uint16_t *>(
        malloc(INITIAL_VERTEX_CAPACITY * sizeof(*s_indices)));
    if (!s_vertices || !s_batches || !s_indices) {
        fprintf(stderr, "ged3d12: could not allocate decoded vertex capture storage\n");
        ged3d12_vertex_capture_shutdown();
        return false;
    }
    s_memo_primitive = -1;
    s_vertex_capacity = INITIAL_VERTEX_CAPACITY;
    s_batch_capacity = INITIAL_BATCH_CAPACITY;
    s_index_capacity = INITIAL_VERTEX_CAPACITY;
    s_index_count = 0;
    s_vertex_count = 0;
    s_batch_count = 0;
    s_capture_failed = 0;
    memset(s_primitive_count, 0, sizeof(s_primitive_count));
    return true;
}

void ged3d12_vertex_capture_shutdown(void) {
    free(s_vertices);
    free(s_batches);
    free(s_indices);
    s_indices = nullptr;
    s_index_count = 0;
    s_index_capacity = 0;
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
    update_batch_bounds(first, 3);
    return 1;
}

static int capture_tri_list_impl(
    const GeVtx *vertices, uint32_t count, int perspective);

int ged3d12_vertex_capture_tri_list(
    const GeVtx *vertices, uint32_t count, int perspective) {
    SrPerfCategory previous = sr_perf_switch(SR_PERF_CAPTURE);
    int result = capture_tri_list_impl(vertices, count, perspective);
    sr_perf_switch(previous);
    return result;
}

static int capture_tri_list_impl(
    const GeVtx *vertices, uint32_t count, int perspective) {
    if (!vertices || !count || count % 3u) return 0;
    uint32_t first;
    if (!begin_primitive(
            GE_D3D12_PRIMITIVE_TRIANGLE, perspective, count, &first)) {
        return 0;
    }
    const GeState *state = ge_state_ptr();
    int flat = !state->shade_gouraud || state->clear;
    for (uint32_t i = 0; i < count; i += 3u) {
        const GeVtx *a = &vertices[i];
        const GeVtx *b = &vertices[i + 1u];
        const GeVtx *c = &vertices[i + 2u];
        const GeVtx *color_a = flat ? c : a;
        const GeVtx *color_b = flat ? c : b;
        put_vertex(a->x, a->y, a->z, a->rw, a->u, a->v, a->fog,
                   color_a->r, color_a->g, color_a->b, color_a->a);
        put_vertex(b->x, b->y, b->z, b->rw, b->u, b->v, b->fog,
                   color_b->r, color_b->g, color_b->b, color_b->a);
        put_vertex(c->x, c->y, c->z, c->rw, c->u, c->v, c->fog,
                   c->r, c->g, c->b, c->a);
    }
    s_primitive_count[GE_D3D12_PRIMITIVE_TRIANGLE] += count / 3u - 1u;
    update_batch_bounds(first, count);
    return 1;
}

static int capture_tri_indexed_impl(
    const GeVtx *table, uint32_t table_count,
    const uint16_t *indices, uint32_t index_count, int perspective);

int ged3d12_vertex_capture_tri_indexed(
    const GeVtx *table, uint32_t table_count,
    const uint16_t *indices, uint32_t index_count, int perspective) {
    SrPerfCategory previous = sr_perf_switch(SR_PERF_CAPTURE);
    int result = capture_tri_indexed_impl(
        table, table_count, indices, index_count, perspective);
    sr_perf_switch(previous);
    return result;
}

static int capture_tri_indexed_impl(
    const GeVtx *table, uint32_t table_count,
    const uint16_t *indices, uint32_t index_count, int perspective) {
    if (!table || !indices || !table_count || table_count > 65536u ||
        !index_count || index_count % 3u) {
        return 0;
    }
    for (uint32_t i = 0; i < index_count; i++)
        if (indices[i] >= table_count) return 0;
    uint32_t first;
    if (!reserve_indices(index_count) ||
        !begin_primitive_ex(
            GE_D3D12_PRIMITIVE_TRIANGLE, perspective, table_count, &first,
            false)) {
        return 0;
    }
    GeD3D12VertexBatch *batch = &s_batches[s_batch_count - 1u];
    batch->flags = GE_D3D12_BATCH_INDEXED;
    batch->count = index_count;
    batch->table_count = table_count;
    batch->index_first = s_index_count;
    for (uint32_t i = 0; i < table_count; i++) {
        const GeVtx *v = &table[i];
        put_vertex(v->x, v->y, v->z, v->rw, v->u, v->v, v->fog,
                   v->r, v->g, v->b, v->a);
    }
    memcpy(&s_indices[s_index_count], indices,
           (size_t)index_count * sizeof(*indices));
    s_index_count += index_count;
    s_primitive_count[GE_D3D12_PRIMITIVE_TRIANGLE] += index_count / 3u - 1u;
    update_batch_bounds(first, table_count);
    return 1;
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
    const GeState *state = ge_state_ptr();
    bool linear =
        (state->tex_filter & 1u) != 0 ||
        ((state->tex_filter >> 8) & 1u) != 0;
    float ua = linear ? u0 : u0 - 0.5f * texel_width;
    float ub = linear ? u1 : u1 - 0.5f * texel_width;
    float va = linear ? v0 : v0 - 0.5f * texel_height;
    float vb = linear ? v1 : v1 - 0.5f * texel_height;

    float z = p1->z;
    float fog = p1->fog;
    put_vertex(xa, ya, z, 1.0f, ua, va, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xb, ya, z, 1.0f, ub, va, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xa, yb, z, 1.0f, ua, vb, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xb, ya, z, 1.0f, ub, va, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xb, yb, z, 1.0f, ub, vb, fog, p1->r, p1->g, p1->b, p1->a);
    put_vertex(xa, yb, z, 1.0f, ua, vb, fog, p1->r, p1->g, p1->b, p1->a);
    update_batch_bounds(first, 6);
    return 1;
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
    update_batch_bounds(first, 6);
    return 1;
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
    update_batch_bounds(first, 6);
    return 1;
}

uint64_t ged3d12_vertex_hash(
    const GeD3D12Vertex *vertices, uint32_t count) {
    if (!vertices && count) return 0;
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
    if (s_index_count) {
        UINT64 index_size = (UINT64)s_index_count * sizeof(*s_indices);
        GeD3D12UploadSlice index_upload = {};
        if (!ged3d12_upload_alloc(index_size, 16, &index_upload)) {
            fprintf(stderr, "ged3d12: could not allocate %llu bytes for indices\n",
                    (unsigned long long)index_size);
            return false;
        }
        memcpy(index_upload.cpu, s_indices, (size_t)index_size);
        out->index_base = index_upload.gpu;
    }

    out->buffer.BufferLocation = upload.gpu;
    out->buffer.SizeInBytes = (UINT)size;
    out->buffer.StrideInBytes = sizeof(*s_vertices);
    out->vertices = s_vertices;
    out->batches = s_batches;
    out->vertex_count = s_vertex_count;
    out->batch_count = s_batch_count;
    memcpy(out->primitive_count, s_primitive_count, sizeof(out->primitive_count));
    out->hash = 0;

    s_vertex_count = 0;
    s_index_count = 0;
    s_batch_count = 0;
    memset(s_primitive_count, 0, sizeof(s_primitive_count));
    return true;
}

void ged3d12_vertex_pending_counts(
    uint32_t *vertex_count, uint32_t *batch_count) {
    if (vertex_count) *vertex_count = s_vertex_count;
    if (batch_count) *batch_count = s_batch_count;
}
