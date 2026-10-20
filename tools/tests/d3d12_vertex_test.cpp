#include "../../src/rt/gpu_d3d12/device.h"
#include "../../src/rt/gpu_d3d12/vertex.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_UPLOAD_SIZE (8u * 1024u * 1024u)
#define TEST_GPU_BASE UINT64_C(0x10000000)

static GeState s_state;
static uint8_t s_upload[TEST_UPLOAD_SIZE];
static UINT64 s_upload_cursor;
static int s_failures;

extern "C" GeState *ge_state_ptr(void) {
    return &s_state;
}

bool ged3d12_upload_alloc(
    UINT64 size, UINT64 alignment, GeD3D12UploadSlice *out) {
    UINT64 offset = (s_upload_cursor + alignment - 1) & ~(alignment - 1);
    if (!out || !size || !alignment || offset > TEST_UPLOAD_SIZE ||
        size > TEST_UPLOAD_SIZE - offset) {
        return false;
    }
    s_upload_cursor = offset + size;
    out->cpu = s_upload + offset;
    out->gpu = TEST_GPU_BASE + offset;
    out->offset = offset;
    out->size = size;
    return true;
}

static void reset_upload(void) {
    memset(s_upload, 0xcd, sizeof(s_upload));
    s_upload_cursor = 3;
}

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    s_failures++;
}

static void check_u32(const char *what, uint32_t got, uint32_t expected) {
    if (got != expected) {
        fprintf(stderr, "FAIL: %s: got 0x%08x, expected 0x%08x\n",
                what, got, expected);
        s_failures++;
    }
}

static void check_u64(const char *what, uint64_t got, uint64_t expected) {
    if (got != expected) {
        fprintf(stderr, "FAIL: %s: got 0x%016llx, expected 0x%016llx\n",
                what, (unsigned long long)got, (unsigned long long)expected);
        s_failures++;
    }
}

static void check_float(const char *what, float got, float expected) {
    if (fabsf(got - expected) > 1e-6f) {
        fprintf(stderr, "FAIL: %s: got %.9g, expected %.9g\n",
                what, got, expected);
        s_failures++;
    }
}

static const GeD3D12Vertex *uploaded_vertices(const GeD3D12VertexFrame *frame) {
    UINT64 offset = frame->buffer.BufferLocation - TEST_GPU_BASE;
    return reinterpret_cast<const GeD3D12Vertex *>(s_upload + offset);
}

static GeVtx vertex(
    float x, float y, float z, float rw, float u, float v, float fog,
    int r, int g, int b, int a) {
    GeVtx out = {x, y, z, rw, u, v, fog, r, g, b, a};
    return out;
}

static void test_triangle(void) {
    reset_upload();
    memset(&s_state, 0, sizeof(s_state));
    s_state.shade_gouraud = 0;
    GeVtx a = vertex(1.1f, 2.2f, 3.0f, 0.25f, 4.0f, 5.0f, 0.5f,
                     1, 2, 3, 4);
    GeVtx b = vertex(6.3f, 7.4f, 8.0f, 0.5f, 9.0f, 10.0f, 0.75f,
                     5, 6, 7, 8);
    GeVtx c = vertex(11.5f, 12.6f, 13.0f, 1.0f, 14.0f, 15.0f, 1.0f,
                     300, -1, 128, 256);
    check_u32("triangle hook remains software", ged3d12_vertex_capture_tri(&a, &b, &c, 1), 0);

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("triangle upload failed");
        return;
    }
    check_u32("triangle vertex count", frame.vertex_count, 3);
    check_u32("triangle batch count", frame.batch_count, 1);
    check_u32("triangle primitive count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_TRIANGLE], 1);
    check_u32("triangle stride", frame.buffer.StrideInBytes, 32);
    check_u32("triangle upload bytes", frame.buffer.SizeInBytes, 96);
    check_u64("triangle upload alignment", frame.buffer.BufferLocation, TEST_GPU_BASE + 16);
    if (!frame.hash) fail("triangle hash was zero");
    check_u32("triangle batch first", frame.batches[0].first, 0);
    check_u32("triangle batch vertices", frame.batches[0].count, 3);
    check_u32("triangle batch kind", frame.batches[0].primitive,
              GE_D3D12_PRIMITIVE_TRIANGLE);
    check_u32("triangle perspective", frame.batches[0].perspective, 1);

    const GeD3D12Vertex *vertices = uploaded_vertices(&frame);
    check_float("triangle snapped x", vertices[0].x, 1.0625f);
    check_float("triangle snapped y", vertices[0].y, 2.1875f);
    check_float("triangle z", vertices[0].z, 3.0f);
    check_float("triangle reciprocal w", vertices[0].rw, 0.25f);
    check_float("triangle u", vertices[0].u, 4.0f);
    check_float("triangle fog", vertices[0].fog, 0.5f);
    check_u32("triangle flat color A", vertices[0].rgba, 0xff8000ffu);
    check_u32("triangle flat color B", vertices[1].rgba, 0xff8000ffu);
    check_u32("triangle flat color C", vertices[2].rgba, 0xff8000ffu);

    reset_upload();
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("empty upload failed");
    } else {
        check_u32("empty upload vertex count", frame.vertex_count, 0);
        check_u32("empty upload batch count", frame.batch_count, 0);
    }
}

static void test_sprites(void) {
    reset_upload();
    GeVtx p0 = vertex(10.9f, 20.9f, 2.0f, 0.5f, 2.0f, 3.0f, 0.25f,
                      1, 2, 3, 4);
    GeVtx p1 = vertex(14.2f, 24.7f, 9.0f, 1.5f, 12.0f, 15.0f, 0.75f,
                      10, 20, 30, 40);
    ged3d12_vertex_capture_sprite(&p0, &p1, 1);

    GeVtx q0 = vertex(30.2f, 40.8f, 4.0f, 0.25f, 2.0f, 6.0f, 0.2f,
                      5, 6, 7, 8);
    GeVtx q1 = vertex(34.9f, 44.1f, 5.0f, 2.0f, 10.0f, 14.0f, 0.4f,
                      50, 60, 70, 80);
    ged3d12_vertex_capture_sprite(&q0, &q1, 0);
    GeVtx empty = q0;
    empty.x = q0.x + 0.1f;
    empty.y = q0.y + 0.1f;
    ged3d12_vertex_capture_sprite(&q0, &empty, 0);

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("sprite upload failed");
        return;
    }
    check_u32("sprite vertex count", frame.vertex_count, 12);
    check_u32("sprite batch count", frame.batch_count, 2);
    check_u32("sprite primitive count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_SPRITE], 2);
    check_u32("perspective sprite flag", frame.batches[0].perspective, 1);
    check_u32("through sprite flag", frame.batches[1].perspective, 0);
    check_u32("through sprite batch first", frame.batches[1].first, 6);

    const GeD3D12Vertex *vertices = uploaded_vertices(&frame);
    check_float("perspective sprite xa", vertices[0].x, 10.0f);
    check_float("perspective sprite ya", vertices[0].y, 20.0f);
    check_float("perspective sprite xb", vertices[1].x, 14.0f);
    check_float("perspective sprite yb", vertices[2].y, 24.0f);
    check_float("perspective sprite ua", vertices[0].u, 3.5f);
    check_float("perspective sprite ub", vertices[1].u, 7.5f);
    check_float("perspective sprite va", vertices[0].v, 5.5f);
    check_float("perspective sprite vb", vertices[2].v, 9.5f);
    check_float("perspective sprite z from p1", vertices[0].z, 9.0f);
    check_float("perspective sprite rw", vertices[0].rw, 1.0f);
    check_u32("perspective sprite color", vertices[0].rgba, 0x281e140au);

    check_float("through sprite ua", vertices[6].u, 1.0f);
    check_float("through sprite ub", vertices[7].u, 9.0f);
    check_float("through sprite va", vertices[6].v, 5.0f);
    check_float("through sprite vb", vertices[8].v, 13.0f);
}

static void test_lines_and_points(void) {
    reset_upload();
    s_state.shade_gouraud = 0;
    GeVtx a = vertex(0.0f, 0.0f, 1.0f, 0.5f, 2.0f, 3.0f, 0.5f,
                     1, 2, 3, 4);
    GeVtx b = vertex(2.0f, 0.0f, 4.0f, 1.0f, 5.0f, 6.0f, 0.75f,
                     10, 20, 30, 40);
    GeVtx p = vertex(4.8f, 8.9f, 7.0f, 1.0f, 8.0f, 9.0f, 1.0f,
                     50, 60, 70, 80);
    ged3d12_vertex_capture_line(&a, &b, 1);
    ged3d12_vertex_capture_point(&p, 0);

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("line/point upload failed");
        return;
    }
    check_u32("line/point vertex count", frame.vertex_count, 12);
    check_u32("line/point batch count", frame.batch_count, 2);
    check_u32("line primitive count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_LINE], 1);
    check_u32("point primitive count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_POINT], 1);
    check_u32("point batch first", frame.batches[1].first, 6);
    check_u32("point batch kind", frame.batches[1].primitive,
              GE_D3D12_PRIMITIVE_POINT);

    const GeD3D12Vertex *vertices = uploaded_vertices(&frame);
    check_float("line first x", vertices[0].x, 0.0f);
    check_float("line first y", vertices[0].y, 0.0f);
    check_float("line far x", vertices[2].x, 3.0f);
    check_u32("line flat color A", vertices[0].rgba, 0x281e140au);
    check_u32("line color B", vertices[2].rgba, 0x281e140au);
    check_float("point floor x", vertices[6].x, 4.0f);
    check_float("point floor y", vertices[6].y, 8.0f);
    check_float("point max x", vertices[10].x, 5.0f);
    check_float("point max y", vertices[10].y, 9.0f);
}

static void test_storage_growth(void) {
    reset_upload();
    s_state.shade_gouraud = 1;
    GeVtx a = vertex(1.0f, 2.0f, 3.0f, 1.0f, 4.0f, 5.0f, 1.0f,
                     1, 2, 3, 4);
    GeVtx b = a;
    GeVtx c = a;
    for (uint32_t i = 0; i < 3000; i++)
        ged3d12_vertex_capture_tri(&a, &b, &c, 1);

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("grown storage upload failed");
        return;
    }
    check_u32("grown vertex count", frame.vertex_count, 9000);
    check_u32("grown batch count", frame.batch_count, 3000);
    check_u32("grown upload size", frame.buffer.SizeInBytes, 9000 * 32);
}

int main(void) {
    if (!ged3d12_vertex_capture_init()) {
        fail("vertex capture initialization failed");
    } else {
        test_triangle();
        test_sprites();
        test_lines_and_points();
        test_storage_growth();
        ged3d12_vertex_capture_shutdown();
    }

    if (s_failures) {
        fprintf(stderr, "D3D12 vertex tests: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("D3D12 vertex tests passed\n");
    return 0;
}
