#!/usr/bin/env python3
"""Unit tests for tools/codegen_ext.py: emit C for chosen instruction words, compile it
with a stand-in CpuState and prefix helpers, run it, and compare the results with Python
models of PPSSPP's interpreter semantics.

    python tools/tests/test_codegen_ext.py [--cc=gcc] [--keep]

Needs a C compiler (gcc or clang) on PATH. Exits non-zero on any mismatch.
"""
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import codegen as cg  # noqa: E402
import codegen_ext as cx  # noqa: E402

# Stand-in runtime: the CpuState fields and prefix helpers the emitted code touches, written
# from the semantics documented in sal063 recomp.h (S/T prefix: swizzle, abs, constant,
# negate; D prefix: saturate 0..1 / -1..1, write mask).
HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct {
    uint32_t r[32];
    union { float v[128]; uint32_t vi[128]; };
    uint32_t vfpuCtrl[16];
} CpuState;
static const float K[8] = {0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f/3.0f, 0.25f, 1.0f/6.0f};
static void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t p) {
    float o[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; i++) o[i] = s->v[idx[i]];
    for (int i = 0; i < n; i++) {
        int sw = (p >> (2 * i)) & 3, ab = (p >> (8 + i)) & 1, k = (p >> (12 + i)) & 1, ng = (p >> (16 + i)) & 1;
        uint32_t b;
        if (k) { memcpy(&b, &K[sw + 4 * ab], 4); }
        else { memcpy(&b, &o[sw], 4); if (ab) b &= 0x7FFFFFFFu; }
        if (ng) b ^= 0x80000000u;
        memcpy(&r[i], &b, 4);
    }
}
static void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t p) {
    for (int i = 0; i < n; i++) {
        int sat = (p >> (2 * i)) & 3;
        if (sat == 1) d[i] = d[i] < 0.0f ? 0.0f : (d[i] > 1.0f ? 1.0f : d[i]);
        else if (sat == 3) d[i] = d[i] < -1.0f ? -1.0f : (d[i] > 1.0f ? 1.0f : d[i]);
    }
    for (int i = 0; i < n; i++)
        if (!((p >> (8 + i)) & 1)) s->v[idx[i]] = d[i];
}
#define sr_begin(...) ((void)0)
#define sr_end(...) ((void)0)
static void dispatch(CpuState *s, uint32_t t) { s->r[2] = 0xD15Fu; s->r[3] = t; }
static void reset(CpuState *s) {
    memset(s, 0, sizeof *s);
    s->vfpuCtrl[0] = 0xe4u; s->vfpuCtrl[1] = 0xe4u;
}
static void dump(const char *name, CpuState *s, const int *idx, int n) {
    printf("%s", name);
    for (int i = 0; i < n; i++) printf(" %08x", s->vi[idx[i]]);
    printf("\n");
}
"""


def f2u(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def f32(x):
    return u2f(f2u(x)) if not math.isnan(x) else x


def size_bits(n):
    return {1: 0, 2: 1 << 7, 3: 1 << 15, 4: (1 << 7) | (1 << 15)}[n]


def enc34(jump, sub, vs, vd, n):
    return (0x34 << 26) | (jump << 21) | (sub << 16) | size_bits(n) | (vs << 8) | vd


class Suite:
    def __init__(self):
        self.tests = []   # (name, c_body, expected_lines)
        self.defs = []    # C definitions placed before main()

    def add(self, name, setup, stmt, out_idx, expected):
        """setup: list of C statements; stmt: emitted C; out_idx: v[] indices to dump;
        expected: list of u32 values (or None to skip a lane)."""
        idx = ",".join(str(i) for i in out_idx)
        body = (f"  {{ CpuState st, *s = &st; reset(s); {' '.join(setup)}\n    {stmt}\n"
                f"    static const int _o[] = {{{idx}}}; dump(\"{name}\", s, _o, {len(out_idx)}); }}\n")
        self.tests.append((name, body, expected))

    def add_gpr(self, name, setup, stmt, reg, expected):
        body = (f"  {{ CpuState st, *s = &st; reset(s); {' '.join(setup)}\n    {stmt}\n"
                f"    printf(\"{name} %08x\\n\", s->r[{reg}]); }}\n")
        self.tests.append((name, body, [expected]))

    def add_cond(self, name, setup, cond, expected):
        body = (f"  {{ CpuState st, *s = &st; reset(s); {' '.join(setup)}\n"
                f"    printf(\"{name} %08x\\n\", (unsigned)({cond})); }}\n")
        self.tests.append((name, body, [1 if expected else 0]))


def setv(idx, vals):
    return [f"s->vi[{i}]=0x{v & 0xFFFFFFFF:08x}u;" for i, v in zip(idx, vals)]


# ---- Python reference models (PPSSPP Interpreter / InterpreterVFPU) ------------------------

def ref_vi2f(bits, imm):
    out = []
    for b in bits:
        iv = b - (1 << 32) if b & 0x80000000 else b
        out.append(f2u(f32(f32(float(iv)) * f32(1.0 / (1 << imm)))))
    return out


def ref_vf2i(x, imm, mode):
    if math.isnan(x):
        return 0x7FFFFFFF
    sv = f32(x * float(1 << imm)) if not math.isinf(x) else x
    if sv > 2147483647.0:
        return 0x7FFFFFFF
    if sv <= -2147483648.0:
        return 0x80000000
    if mode == 16:
        r = math.floor(sv)
        f = sv - r
        if f > 0.5 or (f == 0.5 and r % 2 != 0):
            r += 1
        v = int(r)
    elif mode == 17:
        v = math.floor(sv) if x >= 0 else math.ceil(sv)
    elif mode == 18:
        v = math.ceil(sv)
    else:
        v = math.floor(sv)
    return v & 0xFFFFFFFF


def ref_vx2i(sub, s, n):
    d = [0, 0, 0, 0]
    if sub == 24:
        v = s[0]
        for i in range(4):
            d[i] = (((v >> (8 * i)) & 0xFF) * 0x01010101 & 0xFFFFFFFF) >> 1
        return d
    if sub == 25:
        v = s[0]
        return [(v & 0xFF) << 24 & 0xFFFFFFFF, (v & 0xFF00) << 16 & 0xFFFFFFFF, (v & 0xFF0000) << 8 & 0xFFFFFFFF, v & 0xFF000000]
    lanes = 1 if n == 1 else 2
    for i in range(lanes):
        if sub == 26:
            d[2 * i], d[2 * i + 1] = (s[i] & 0xFFFF) << 15, (s[i] & 0xFFFF0000) >> 1
        else:
            d[2 * i], d[2 * i + 1] = (s[i] & 0xFFFF) << 16, s[i] & 0xFFFF0000
    return d[:2 if n == 1 else 4]


def ref_vi2x(sub, source, n):
    signed = [
        value - (1 << 32) if value & 0x80000000 else value
        for value in (source + [0, 0, 0, 0])[:4]
    ]
    unsigned = [value & 0xFFFFFFFF for value in signed]
    if sub == 28:
        packed = 0
        for lane, value in enumerate(signed):
            if value < 0:
                value = 0
            packed |= ((value >> 23) & 0xFF) << (lane * 8)
        return [packed]
    if sub == 29:
        packed = 0
        for lane, value in enumerate(unsigned):
            packed |= (value >> 24) << (lane * 8)
        return [packed]
    result = []
    for lane in range((n + 1) // 2):
        low = signed[lane * 2]
        high = signed[lane * 2 + 1]
        if sub == 30:
            low = max(low, 0) >> 15
            high = max(high, 0) >> 15
        else:
            low = unsigned[lane * 2] >> 16
            high = unsigned[lane * 2 + 1] >> 16
        result.append((low & 0xFFFF) | ((high & 0xFFFF) << 16))
    return result


def ref_vsgn(x):
    if math.isnan(x):
        b = f2u(x)
    else:
        b = f2u(x - 0.0)
    if b & 0x7F800000 == 0:
        return f2u(0.0)
    return f2u(-1.0) if b >> 31 else f2u(1.0)


def bitrev(x):
    return int(f"{x:032b}"[::-1], 2)


def build(suite, rnd):
    # BSHFL
    for val in (0x12345678, 0x80000001, 0xFFFF0000, 0, 0xA5A5A5A5):
        w = (0x1F << 26) | (5 << 16) | (6 << 11) | (0x14 << 6) | 0x20  # bitrev $a2, $a1
        suite.add_gpr(f"bitrev_{val:08x}", [f"s->r[5]=0x{val:08x}u;"], cx.effect(0, w)[0], 6, bitrev(val))
        w = (0x1F << 26) | (5 << 16) | (6 << 11) | (0x03 << 6) | 0x20  # wsbw
        suite.add_gpr(f"wsbw_{val:08x}", [f"s->r[5]=0x{val:08x}u;"], cx.effect(0, w)[0], 6,
                      struct.unpack(">I", struct.pack("<I", val))[0])
    # cache is a no-op that must still compile
    suite.add_gpr("cache", ["s->r[4]=7;"], cx.effect(0, 0xBC9B0000 | (4 << 21))[0], 4, 7)

    # vi2f over sizes, immediates and register layouts (incl. a transposed one)
    for n in (1, 2, 3, 4):
        for imm in (0, 1, 8, 15, 31):
            for vs, vd in ((0x00, 0x04), (0x21 if n in (1, 2, 4) else 0x01, 0x05)):
                bits = [rnd.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF]) if rnd.random() < 0.3
                        else rnd.getrandbits(32) for _ in range(n)]
                si, di = cx.vreg_indices(vs, n), cx.vreg_indices(vd, n)
                w = enc34(20, imm, vs, vd, n)
                suite.add(f"vi2f_n{n}_i{imm}_s{vs:02x}", setv(si, bits), cx.effect(0, w)[0], di, ref_vi2f(bits, imm))
    # vi2f honours the D prefix: lane 1 write-masked, lane 0 saturated to [0,1]
    si, di = cx.vreg_indices(0, 2), cx.vreg_indices(4, 2)
    suite.add("vi2f_dprefix", setv(si, [5, 9]) + setv(di, [0xDEAD, 0xBEEF]) + ["s->vfpuCtrl[2]=0x201u;"],
              cx.effect(0, enc34(20, 0, 0, 4, 2))[0], di, [f2u(1.0), 0xBEEF])

    # vf2i: rounding modes, ties, NaN, saturation; D prefix sat must NOT apply (mask only)
    cases = [2.5, 3.5, -2.5, -3.5, 0.49999997, -0.5, 1.5, 123.75, -123.25, float("nan"), 3e9, -3e9,
             float("inf"), float("-inf"), 0.0, -0.0]
    for mode in (16, 17, 18, 19):
        for imm in (0, 4):
            for k in range(0, len(cases), 4):
                xs = cases[k:k + 4]
                si, di = cx.vreg_indices(0, 4), cx.vreg_indices(4, 4)
                w = enc34(mode, imm, 0, 4, 4)
                suite.add(f"vf2i_m{mode}_i{imm}_{k}", setv(si, [f2u(x) for x in xs]), cx.effect(0, w)[0], di,
                          [ref_vf2i(x, imm, mode) for x in xs])
    si, di = cx.vreg_indices(0, 2), cx.vreg_indices(4, 2)
    suite.add("vf2i_nosat", setv(si, [f2u(7.0), f2u(9.0)]) + setv(di, [0, 0x1234]) + ["s->vfpuCtrl[2]=0x205u;"],
              cx.effect(0, enc34(16, 0, 0, 4, 2))[0], di, [7, 0x1234])

    # vx2i family
    for sub, n in ((24, 1), (24, 4), (25, 1), (26, 1), (26, 2), (26, 4), (27, 1), (27, 2), (27, 3)):
        for _ in range(3):
            s = [rnd.getrandbits(32) for _ in range(n)]
            oz = 4 if sub in (24, 25) or n >= 2 else 2
            si, di = cx.vreg_indices(0, n), cx.vreg_indices(8, oz)
            w = enc34(1, sub, 0, 8, n)
            suite.add(f"vx2i_{sub}_n{n}_{s[0]:08x}", setv(si, s), cx.effect(0, w)[0], di, ref_vx2i(sub, s, n))

    # VFPU7 vi2uc/vi2c/vi2us/vi2s: integer lane packing after source prefixes.
    vi2x_cases = (
        (28, 4, [0x7FFFFFFF, 0x80000000, 0x01000000, 0xFFFFFFFF]),
        (29, 4, [0x80000000, 0x7F000000, 0xFF000000, 0x01000000]),
        (30, 3, [0x7FFFFFFF, 0x80000000, 0x40000000]),
        (31, 1, [0x89ABCDEF]),
        (31, 2, [0x12345678, 0xFEDCBA98]),
        (31, 4, [0x12345678, 0xFEDCBA98, 0x80000000, 0x7FFFFFFF]),
    )
    for sub, n, source in vi2x_cases:
        out_n = 1 if sub in (28, 29) else (n + 1) // 2
        si = cg.vreg_indices(0, n)
        di = cg.vreg_indices(8, out_n)
        suite.add(
            f"vi2x7_{sub}_n{n}",
            setv(si, source),
            cg.vfpu_effect(0, enc34(1, sub, 0, 8, n))[0],
            di,
            ref_vi2x(sub, source, n),
        )

    # Source swizzle swaps lanes 0/1 before signed-halfword packing.
    source = [0x11112222, 0x33334444, 0x55556666, 0x77778888]
    si, di = cg.vreg_indices(0, 4), cg.vreg_indices(8, 2)
    suite.add(
        "vi2x7_sprefix",
        setv(si, source) + ["s->vfpuCtrl[0]=0xe1u;"],
        cg.vfpu_effect(0, enc34(1, 31, 0, 8, 4))[0],
        di,
        ref_vi2x(31, [source[1], source[0], source[2], source[3]], 4),
    )

    # D saturation applies to the packed float bit pattern.
    si, di = cg.vreg_indices(0, 4), cg.vreg_indices(8, 1)
    source = [0, 0, 0, 0x40000000]
    suite.add(
        "vi2x7_dsat",
        setv(si, source) + ["s->vfpuCtrl[2]=1u;"],
        cg.vfpu_effect(0, enc34(1, 29, 0, 8, 4))[0],
        di,
        [f2u(1.0)],
    )

    # D write mask preserves packed destination lane 1.
    source = [0x11112222, 0x33334444, 0x55556666, 0x77778888]
    si, di = cg.vreg_indices(0, 4), cg.vreg_indices(8, 2)
    expected = ref_vi2x(31, source, 4)
    suite.add(
        "vi2x7_dmask",
        setv(si, source)
        + setv(di, [0xDEADBEEF, 0xCAFEBABE])
        + ["s->vfpuCtrl[2]=0x200u;"],
        cg.vfpu_effect(0, enc34(1, 31, 0, 8, 4))[0],
        di,
        [expected[0], 0xCAFEBABE],
    )

    # vfad: horizontal sum, sequential float32 accumulation from +0
    for n in (2, 3, 4):
        for _ in range(4):
            xs = [f32(rnd.uniform(-1000, 1000)) for _ in range(n)]
            acc = 0.0
            for x in xs:
                acc = f32(acc + f32(x * 1.0))
            si, di = cx.vreg_indices(0, n), cx.vreg_indices(0x10, 1)
            suite.add(f"vfad_n{n}_{f2u(xs[0]):08x}", setv(si, [f2u(x) for x in xs]),
                      cx.effect(0, enc34(2, 6, 0, 0x10, n))[0], di, [f2u(acc)])
    # vfad with a user T abs prefix: constant 1.0 becomes 1/3 (PPSSPP keeps abs on the rewritten constant)
    si, di = cx.vreg_indices(0, 2), cx.vreg_indices(0x10, 1)
    third = f32(1.0 / 3.0)
    suite.add("vfad_tabs", setv(si, [f2u(3.0), f2u(6.0)]) + ["s->vfpuCtrl[1]=0xe4u|0x300u;"],
              cx.effect(0, enc34(2, 6, 0, 0x10, 2))[0], di, [f2u(f32(f32(0.0 + f32(3.0 * third)) + f32(6.0 * third)))])

    # vsgn
    xs = [5.0, -0.25, 0.0, -0.0, u2f(0x00000001), u2f(0x80000010), float("inf"), u2f(0xFFC00000)]
    for k in range(0, len(xs), 4):
        si, di = cx.vreg_indices(0, 4), cx.vreg_indices(4, 4)
        suite.add(f"vsgn_{k}", setv(si, [f2u(x) for x in xs[k:k + 4]]), cx.effect(0, enc34(2, 10, 0, 4, 4))[0], di,
                  [ref_vsgn(x) for x in xs[k:k + 4]])

    # vmscl 3x3 and 4x4: every element times the scalar
    for side in (3, 4):
        vs, vd, vt = 0x00, 0x04, 0x0B  # M000 -> M100, scalar S203
        m = [f32(rnd.uniform(-10, 10)) for _ in range(side * side)]
        sc = f32(rnd.uniform(-3, 3))
        setup = [f"s->v[{cx.mreg_index(vs, side, a, b)}]={m[a * side + b]!r}f;" for a in range(side) for b in range(side)]
        setup.append(f"s->v[{cx.vreg_indices(vt, 1)[0]}]={sc!r}f;")
        w = (0x3C << 26) | (4 << 23) | (vt << 16) | size_bits(side) | (vs << 8) | vd
        out = [cx.mreg_index(vd, side, a, b) for a in range(side) for b in range(side)]
        suite.add(f"vmscl_{side}", setup, cx.effect(0, w)[0], out, [f2u(f32(x * sc)) for x in m])

    # vmmov captures the complete source before writing an overlapping destination.
    for side, vd in ((2, 0x01), (3, 0x01), (4, 0x04)):
        vs = 0x00
        source = [f32(float(i + 1)) for i in range(side * side)]
        source_indices = [
            cx.mreg_index(vs, side, j, i)
            for j in range(side)
            for i in range(side)
        ]
        dest_indices = [
            cx.mreg_index(vd, side, j, i)
            for j in range(side)
            for i in range(side)
        ]
        setup = setv(source_indices, [f2u(value) for value in source])
        word = (
            (0x3C << 26)
            | (28 << 21)
            | size_bits(side)
            | (vs << 8)
            | vd
        )
        suite.add(
            f"vmmov_{side}",
            setup,
            cg.vfpu_effect(0, word)[0],
            dest_indices,
            [f2u(value) for value in source],
        )

    # Matrix S/D prefixes affect only the final row, including saturation and masking.
    side, vs, vd = 3, 0x00, 0x04
    source = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, -2.0, 2.0, 0.5]
    source_indices = [
        cx.mreg_index(vs, side, j, i)
        for j in range(side)
        for i in range(side)
    ]
    dest_indices = [
        cx.mreg_index(vd, side, j, i)
        for j in range(side)
        for i in range(side)
    ]
    word = (
        (0x3C << 26)
        | (28 << 21)
        | size_bits(side)
        | (vs << 8)
        | vd
    )
    setup = (
        setv(source_indices, [f2u(value) for value in source])
        + setv(dest_indices, [f2u(-9.0)] * len(dest_indices))
        + ["s->vfpuCtrl[0]=0xe1u;", "s->vfpuCtrl[2]=0x40du;"]
    )
    suite.add(
        "vmmov_prefix",
        setup,
        cg.vfpu_effect(0, word)[0],
        dest_indices,
        [
            *[f2u(value) for value in source[:6]],
            f2u(1.0),
            f2u(-1.0),
            f2u(-9.0),
        ],
    )

    # vnop / vflush (opcode 0x3F): 0xFFFFxxxx keeps the S prefix, other encodings eat it
    for w, want in ((0xFFFF0000, 0x1B), (0xFFFF040D, 0x1B), (0xFC000000, 0xE4)):
        suite.add_cond(f"vflush_{w:08x}", ["s->vfpuCtrl[0]=0x1Bu;", cx.effect(0, w)[0]], "s->vfpuCtrl[0]", None)
        suite.tests[-1] = (suite.tests[-1][0], suite.tests[-1][1], [want])

    # bv*: condition on VFPU_CC bit imm3
    for cc in (0x00, 0x01, 0x10, 0x3F, 0x2A):
        for imm3 in range(6):
            for tf in (0, 1):
                w = (0x12 << 26) | (8 << 21) | (imm3 << 18) | (tf << 16) | 0x10
                want = ((cc >> imm3) & 1) == tf
                suite.add_cond(f"bv_cc{cc:02x}_b{imm3}_t{tf}", [f"s->vfpuCtrl[3]=0x{cc:x}u;"], cx.vbranch_cond(w), want)
    # Jump-table switch: target read before the delay slot, which advances the register
    # (the native-call thunk does exactly this); unknown targets fall to dispatch().
    tbl = {"targets": [0x100, 0x108, 0x100, 0x110]}
    lines = cx.emit_switch_jr(0xF0, (12 << 21) | 8, "    s->r[12] += 8;", tbl, known={0x110})
    suite.defs.append("static void f_00000110(CpuState *s) { s->r[2] = 3; }\n"
                  "static void sw_fn(CpuState *s) {\n" + "\n".join(lines) + "\n"
                  "  L_00000100: s->r[2] = 1; return;\n  L_00000108: s->r[2] = 2; return;\n}\n")
    for tgt, want in ((0x100, 1), (0x108, 2), (0x110, 3), (0x104, 0xD15F)):
        suite.add_gpr(f"switch_{tgt:x}", [f"s->r[12]=0x{tgt:x}u; sw_fn(s);"], "(void)0;", 2, want)
    suite.add_gpr("switch_delay_slot", ["s->r[12]=0x100u; sw_fn(s);"], "(void)0;", 12, 0x108)
    jn = cx.emit_jal_next(0x200, 0x0C000082, "    s->r[4] = 9;")
    suite.add_gpr("jal_next_link", [], " ".join(x.strip() for x in jn), 31, 0x208)

    assert cx.is_vbranch(0x49000010) and not cx.vbranch_likely(0x49010010) and cx.vbranch_likely(0x49030010)


def main(argv):
    cc = next((a.split("=", 1)[1] for a in argv[1:] if a.startswith("--cc=")), None) \
        or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        print("SKIP: no C compiler on PATH")
        return 0
    suite = Suite()
    build(suite, random.Random(1234))
    src = HARNESS + cx.PREAMBLE + "\n" + "".join(suite.defs) + "\nint main(void) {\n" + "".join(b for _, b, _ in suite.tests) + "  return 0;\n}\n"
    tmp = tempfile.mkdtemp(prefix="kzl_cgext_")
    c_path, exe = os.path.join(tmp, "t.c"), os.path.join(tmp, "t.exe")
    with open(c_path, "w") as f:
        f.write(src)
    r = subprocess.run([cc, "-std=gnu11", "-O1", "-ffp-contract=off", "-Wall", "-Wno-unused", c_path, "-o", exe, "-lm"],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:4000])
        print("FAIL: compile error; source kept at", c_path)
        return 1
    out = subprocess.run([exe], capture_output=True, text=True).stdout.splitlines()
    got = {ln.split()[0]: [int(x, 16) for x in ln.split()[1:]] for ln in out}
    fails = 0
    for name, _, expected in suite.tests:
        g = got.get(name)
        if g is None or any(e is not None and e != v for e, v in zip(expected, g)) or len(g) != len(expected):
            fails += 1
            print(f"FAIL {name}: got {[f'{x:08x}' for x in g or []]} want {[f'{x:08x}' if x is not None else '--' for x in expected]}")
    print(f"{len(suite.tests) - fails}/{len(suite.tests)} passed ({cc})")
    if "--keep" not in argv:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
