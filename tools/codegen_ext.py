"""KZL extensions to the sal063 Allegrex -> C codegen.  -> T-2.1 ... T-2.6, F-016, F-017

Covers every encoding the sal063 codegen (da17b0e) traps on in KZL.PRX, plus a jump-table
switch emitter. Each emitter returns the same shape as sal063's effect():
(c_statement, store_addr_expr_or_None, store_size), or None when the word is not one of ours,
so the codegen can try these first and fall back to its own decoder.

Semantics follow PPSSPP's interpreter (Core/MIPS/Interpreter*.cpp) and sal063's conventions:
sr_vread() applies the S/T prefix, sr_vwrite() the D prefix (saturate + write mask), the
prefixes are eaten after every VFPU op, and vfpuCtrl[3] is the VFPU condition-code register.

Instruction counts in KZL.PRX v1.20, for reference:
    bvf/bvt/bvfl/bvtl 53 | vi2f 37 | vfad 37 | bitrev 22 | vuc2i 22 | wsbw 7 | vus2i 4
    vs2i 4 | vmscl 4 | vf2in 1 | vsgn 1 | cache 1
Reached only once switch cases are translated: vasin (VV2Op 23, emitted by codegen.py with
sr_vfpu_asin from src/rt/vfpu_asin.c) in 13 functions, vnop/vflush (opcode 0x3F) in 18.
"""
import struct

R = lambda i: "0u" if i == 0 else f"s->r[{i}]"  # noqa: E731


def rs(w): return (w >> 21) & 0x1F
def rt(w): return (w >> 16) & 0x1F
def rd(w): return (w >> 11) & 0x1F
def sa(w): return (w >> 6) & 0x1F
def s16(w): return (w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF


def wr(i, expr):
    return "(void)0;" if i == 0 else f"s->r[{i}] = {expr};"


def vec_size(w):
    return (((w >> 7) & 1) | ((w >> 14) & 2)) + 1


def vreg_indices(reg, size):
    # Physical v[] indices of a VFPU vector register (PPSSPP voffset addressing; same formula
    # as sal063 codegen.vreg_indices).
    mtx, col, transpose = (reg >> 2) & 7, reg & 3, (reg >> 5) & 1
    if size == 1:
        transpose, row = 0, (reg >> 5) & 3
    elif size == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    return [mtx * 16 + ((row + i) & 3) * 4 + col if transpose else mtx * 16 + col * 4 + ((row + i) & 3)
            for i in range(size)]


def mreg_index(reg, side, j, i):
    # Physical v[] index of matrix element (column j, row i) (PPSSPP ReadMatrix).
    mtx, col, transpose = (reg >> 2) & 7, reg & 3, (reg >> 5) & 1
    if side == 1:
        transpose, row = 0, (reg >> 5) & 3
    elif side == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    if transpose:
        return mtx * 16 + ((row + i) & 3) * 4 + ((col + j) & 3)
    return mtx * 16 + ((col + j) & 3) * 4 + ((row + i) & 3)


def _arr(idx):
    return "(const uint8_t[]){" + ",".join(str(x) for x in idx) + "}"


def _flit(v):
    s = f"{v:.9g}"
    if "e" not in s and "." not in s and "n" not in s:
        s += ".0"
    return s + "f"


_EAT = " s->vfpuCtrl[0]=0xe4u; s->vfpuCtrl[1]=0xe4u; s->vfpuCtrl[2]=0u;"
# T prefix rewritten to constants (swizzle removed, constant bits set), keeping the user's abs
# and negate bits, as PPSSPP's VFPURewritePrefix does for vfad / vsgn / vocp.
_T_ONE = "(s->vfpuCtrl[1]&~0xF0FFu)|0x55u|0xF000u"
_T_ZERO = "(s->vfpuCtrl[1]&~0xF0FFu)|0xF000u"

# C helpers the extended codegen needs once per translation unit.
PREAMBLE = """static inline uint32_t sr_bitrev32(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    x = ((x >> 8) & 0x00FF00FFu) | ((x & 0x00FF00FFu) << 8);
    return (x >> 16) | (x << 16);
}
static inline int32_t sr_vf2i(float x, float mult, int mode) {
    /* PPSSPP Int_Vf2i: NaN -> INT_MAX, saturate, then round per mode (16 n, 17 z, 18 u, 19 d). */
    if (isnan(x)) return 0x7FFFFFFF;
    double sv = (double)(x * mult);
    if (sv > 2147483647.0) return 0x7FFFFFFF;
    if (sv <= -2147483648.0) return (int32_t)0x80000000u;
    switch (mode) {
        case 16: { double r = floor(sv), f = sv - r;  /* round half to even */
                   if (f > 0.5 || (f == 0.5 && fmod(r, 2.0) != 0.0)) r += 1.0;
                   return (int32_t)r; }
        case 17: return x >= 0.0f ? (int32_t)floor(sv) : (int32_t)ceil(sv);
        case 18: return (int32_t)ceil(sv);
        default: return (int32_t)floor(sv);
    }
}"""


def _block(body):
    return "{ " + body + " }", None, 0


# ---- SPECIAL3 BSHFL and cache -------------------------------------------------------------

def alu_effect(addr, w):
    op = w >> 26
    if op == 0x1F and w & 0x3F == 0x20:
        sub = sa(w)
        if sub == 0x03:  # wsbw: byte-swap the word
            return wr(rd(w), f"__builtin_bswap32({R(rt(w))})"), None, 0
        if sub == 0x14:  # bitrev: reverse all 32 bits
            return wr(rd(w), f"sr_bitrev32({R(rt(w))})"), None, 0
        return None
    if op == 0x2F:  # cache: no data cache to maintain on the host
        return "(void)0; /* cache */", None, 0
    return None


# ---- VFPU ---------------------------------------------------------------------------------

def vfpu_effect(addr, w):
    op = w >> 26
    vd, vs, vt = w & 0x7F, (w >> 8) & 0x7F, (w >> 16) & 0x7F
    n = vec_size(w)
    if op == 0x34:
        jump = (w >> 21) & 0x1F
        if 16 <= jump <= 19:  # vf2in/vf2iz/vf2iu/vf2id: d = int(s * 2^imm); mask only, no saturate
            imm = (w >> 16) & 0x1F
            body = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(vreg_indices(vs, n))},{n},s->vfpuCtrl[0]); "
                    f"for(int _i=0;_i<{n};_i++){{ int32_t _v=sr_vf2i(_s[_i],{_flit(float(1 << imm))},{jump}); "
                    f"memcpy(&_d[_i],&_v,4); }} "
                    f"sr_vwrite(s,{_arr(vreg_indices(vd, n))},_d,{n},s->vfpuCtrl[2]&~0xFFu);{_EAT}")
            return _block(body)
        if jump == 20:  # vi2f: d = float(int bits of s) / 2^imm
            imm = (w >> 16) & 0x1F
            mult = _flit(struct.unpack("<f", struct.pack("<f", 1.0 / (1 << imm)))[0])
            body = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(vreg_indices(vs, n))},{n},s->vfpuCtrl[0]); "
                    f"for(int _i=0;_i<{n};_i++){{ int32_t _v; memcpy(&_v,&_s[_i],4); _d[_i]=(float)_v*{mult}; }} "
                    f"sr_vwrite(s,{_arr(vreg_indices(vd, n))},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return _block(body)
        if jump == 1:  # VFPU7 group
            sub = (w >> 16) & 0x1F
            if 24 <= sub <= 27:
                return _vx2i(sub, vd, vs, n)
            return None
        if jump == 2:  # VFPU9 group
            sub = (w >> 16) & 0x1F
            si = vreg_indices(vs, n)
            if sub == 6:  # vfad: d.s = sum of the lanes (T forced to constant 1.0)
                body = (f"float _s[4],_t[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                        f"sr_vread(_t,s,{_arr(si)},{n},{_T_ONE}); "
                        f"float _d=0.0f; for(int _i=0;_i<{n};_i++) _d+=_s[_i]*_t[_i]; "
                        f"float _dd[1]={{_d}}; sr_vwrite(s,{_arr(vreg_indices(vd, 1))},_dd,1,s->vfpuCtrl[2]);{_EAT}")
                return _block(body)
            if sub == 10:  # vsgn: sign of (s - t), T forced to constant 0; denormal/zero -> 0
                body = (f"float _s[4],_t[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                        f"sr_vread(_t,s,{_arr(si)},{n},{_T_ZERO}); "
                        f"for(int _i=0;_i<{n};_i++){{ float _x=isnan(_s[_i])?_s[_i]:(isnan(_t[_i])?_t[_i]:_s[_i]-_t[_i]); "
                        f"uint32_t _b; memcpy(&_b,&_x,4); "
                        f"_d[_i]=(_b&0x7F800000u)==0?0.0f:((_b>>31)?-1.0f:1.0f); }} "
                        f"sr_vwrite(s,{_arr(vreg_indices(vd, n))},_d,{n},s->vfpuCtrl[2]);{_EAT}")
                return _block(body)
            return None
        return None
    if op == 0x3C and (w >> 23) & 7 == 4:  # vmscl: matrix * scalar
        side = n
        sc = vreg_indices(vt, 1)[0]
        reads = " ".join(f"float _m{a}_{b}=s->v[{mreg_index(vs, side, a, b)}]*_sc;"
                         for a in range(side) for b in range(side))
        writes = " ".join(f"s->v[{mreg_index(vd, side, a, b)}]=_m{a}_{b};"
                          for a in range(side) for b in range(side))
        return _block(f"float _sc=s->v[{sc}]; {reads} {writes}{_EAT}")
    return None


def _vx2i(sub, vd, vs, n):
    """vuc2i / vc2i / vus2i / vs2i (PPSSPP Int_Vx2i): widen packed 8/16-bit lanes to 32-bit."""
    si = vreg_indices(vs, n)
    head = f"float _s[4]; uint32_t _u[4]={{0,0,0,0}}, _x[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); memcpy(_x,_s,{4 * n}); "
    if sub == 24:    # vuc2i: unsigned bytes -> 31-bit, replicating bits to saturate
        oz, calc = 4, "for(int _i=0;_i<4;_i++) _u[_i]=(uint32_t)(((_x[0]>>(8*_i))&0xFFu)*0x01010101u)>>1; "
    elif sub == 25:  # vc2i: signed bytes into the top byte
        oz, calc = 4, "_u[0]=(_x[0]&0xFFu)<<24; _u[1]=(_x[0]&0xFF00u)<<16; _u[2]=(_x[0]&0xFF0000u)<<8; _u[3]=_x[0]&0xFF000000u; "
    else:            # vus2i (26) / vs2i (27): halfwords -> words
        lanes = 1 if n == 1 else 2
        oz = 2 if n == 1 else 4
        if sub == 26:
            calc = f"for(int _i=0;_i<{lanes};_i++){{ _u[2*_i]=(_x[_i]&0xFFFFu)<<15; _u[2*_i+1]=(_x[_i]&0xFFFF0000u)>>1; }} "
        else:
            calc = f"for(int _i=0;_i<{lanes};_i++){{ _u[2*_i]=(_x[_i]&0xFFFFu)<<16; _u[2*_i+1]=_x[_i]&0xFFFF0000u; }} "
    tail = (f"float _d[4]; memcpy(_d,_u,16); "
            f"sr_vwrite(s,{_arr(vreg_indices(vd, oz))},_d,{oz},s->vfpuCtrl[2]);{_EAT}")
    return _block(head + calc + tail)


def effect(addr, w):
    """Try the KZL extensions; None means 'not ours, use the base decoder'."""
    op = w >> 26
    if op in (0x1F, 0x2F):
        return alu_effect(addr, w)
    if op in (0x34, 0x3C):
        return vfpu_effect(addr, w)
    if op == 0x3F:  # vflush / vnop / vsync: no-ops; only 0xFFFFxxxx keeps the prefixes (PPSSPP Int_Vflush)
        return ("(void)0; /* vnop */" if (w & 0xFFFF0000) == 0xFFFF0000 else "{" + _EAT + " }"), None, 0
    return None


# ---- VFPU condition branches (bvf / bvt / bvfl / bvtl) --------------------------------------

def is_vbranch(w):
    return (w >> 26) == 0x12 and rs(w) == 8


def vbranch_cond(w):
    imm3 = (w >> 18) & 7
    want = (w >> 16) & 1  # 0 = bvf (branch if clear), 1 = bvt (branch if set)
    return f"(((s->vfpuCtrl[3] >> {imm3}) & 1u) == {want}u)"


def vbranch_likely(w):
    return bool((w >> 17) & 1)


# ---- Jump tables ------------------------------------------------------------------------------

def load_jumptables(path):
    """research/kzl_jumptables.toml -> {jr_addr: entry}"""
    import tomllib
    with open(path, "rb") as f:
        return {t["jr"]: t for t in tomllib.load(f).get("jumptable", [])}


def emit_switch_jr(pc, w, delay_line, table, known):
    """C lines for `jr rX` at pc that goes through a recovered jump table.

    The target is read before the delay slot runs (the slot may overwrite rX). Case targets
    that the analyzer also lists as function entries become tail calls; anything else that
    reaches the default goes through dispatch() as before, so a wrong table fails loudly.
    """
    reg = rs(w)
    lines = [f"    sr_begin(s, 0x{pc:08x}u, 0x{w:08x}u); sr_end(s, 0u, 0);",
             f"    {{ uint32_t _t = {R(reg)};",
             f"   {delay_line}",
             "      switch (_t) {"]
    for t in sorted(set(table["targets"])):
        if t in known:
            lines.append(f"        case 0x{t:08x}u: f_{t:08x}(s); return;")
        else:
            lines.append(f"        case 0x{t:08x}u: goto L_{t:08x};")
    lines.append("        default: dispatch(s, _t); return;")
    lines.append("      } }")
    return lines


def switch_labels(table, known):
    """Intra-function labels a recovered table adds (targets that are not function entries)."""
    return {t for t in table["targets"] if t not in known}


def load_overrides(path):
    """config/kzl_overrides.toml -> (computed gotos keyed by jr, set of jal-next sites).
    Computed gotos have the same shape as jump tables, so emit_switch_jr() handles both."""
    import tomllib
    with open(path, "rb") as f:
        cfg = tomllib.load(f)
    gotos = {g["jr"]: g for g in cfg.get("computed_goto", [])}
    return gotos, {j["at"] for j in cfg.get("jal_next", [])}


def emit_jal_next(pc, w, delay_line):
    """`jal .+8` (get-PC idiom): link only, then fall through into the delay slot's successor."""
    return [f"    sr_begin(s, 0x{pc:08x}u, 0x{w:08x}u); s->r[31] = 0x{(pc + 8) & 0xFFFFFFFF:08x}u; sr_end(s, 0u, 0);",
            delay_line]
