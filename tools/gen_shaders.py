#!/usr/bin/env python3
"""gen_shaders.py – hand-assemble minimal SPIRV shaders for VoxelStation13.

Vertex shader  : pos(vec3) + normal(vec3) → gl_Position + fragNormal
                 MVP matrix via push constant (64 bytes).
Fragment shader : fragNormal(vec3) → simple Lambertian diffuse colour.

Outputs C++ header files to src/render/shaders/.
"""

import struct, pathlib

# ── SPIRV constants ────────────────────────────────────────────────────────────
MAGIC   = 0x07230203
VERSION = 0x00010000   # SPIRV 1.0

# ExecutionModel
EXM_VERTEX   = 0
EXM_FRAGMENT = 4

# AddressingModel / MemoryModel
ADR_LOGICAL = 0
MEM_GLSL450 = 1

# ExecutionMode
EM_ORIGIN_UPPER_LEFT = 7

# StorageClass
SC_INPUT          = 1
SC_OUTPUT         = 3
SC_PUSH_CONSTANT  = 9

# Decoration
DEC_BLOCK         = 2
DEC_COL_MAJOR     = 5
DEC_MATRIX_STRIDE = 7
DEC_BUILT_IN      = 11
DEC_LOCATION      = 30
DEC_OFFSET        = 35

# BuiltIn
BI_POSITION  = 0

# FunctionControl
FC_NONE = 0

# Capability
CAP_SHADER = 1

# Opcodes
OP_CAPABILITY          = 17
OP_MEMORY_MODEL        = 14
OP_ENTRY_POINT         = 15
OP_EXECUTION_MODE      = 16
OP_DECORATE            = 71
OP_MEMBER_DECORATE     = 72
OP_TYPE_VOID           = 19
OP_TYPE_FLOAT          = 22
OP_TYPE_VECTOR         = 23
OP_TYPE_MATRIX         = 24
OP_TYPE_STRUCT         = 30
OP_TYPE_POINTER        = 32
OP_TYPE_FUNCTION       = 33
OP_TYPE_INT            = 21
OP_CONSTANT            = 43
OP_CONSTANT_COMPOSITE  = 44
OP_VARIABLE            = 59
OP_ACCESS_CHAIN        = 65
OP_LOAD                = 61
OP_STORE               = 62
OP_COMPOSITE_CONSTRUCT = 80
OP_COMPOSITE_EXTRACT   = 81
OP_MATRIX_TIMES_VECTOR = 145
OP_DOT                 = 148
OP_F_ADD               = 129
OP_F_MUL               = 133
OP_FUNCTION            = 54
OP_FUNCTION_END        = 56
OP_LABEL               = 248
OP_RETURN              = 253


def f2w(f):
    """Encode a Python float as a SPIRV 32-bit word."""
    return struct.unpack('<I', struct.pack('<f', float(f)))[0]


def str_words(s):
    """Encode a C string (null-terminated, 4-byte padded) as SPIRV words."""
    b = s.encode('ascii') + b'\0'
    while len(b) % 4:
        b += b'\0'
    return [struct.unpack_from('<I', b, i)[0] for i in range(0, len(b), 4)]


class Spirv:
    def __init__(self):
        self.words = []
        self._id   = 1

    def new(self):
        r = self._id
        self._id += 1
        return r

    def emit(self, op, *args):
        wc = 1 + len(args)
        self.words.append(wc << 16 | op)
        self.words.extend(args)

    def entry_point(self, exec_model, func_id, name, *interface_ids):
        sw = str_words(name)
        wc = 1 + 1 + 1 + len(sw) + len(interface_ids)
        self.words.append(wc << 16 | OP_ENTRY_POINT)
        self.words.append(exec_model)
        self.words.append(func_id)
        self.words.extend(sw)
        self.words.extend(interface_ids)

    def build(self):
        hdr = [MAGIC, VERSION, 0, self._id, 0]
        raw = hdr + self.words
        return struct.pack(f'<{len(raw)}I', *raw)


# ── Vertex shader ──────────────────────────────────────────────────────────────
def build_vertex():
    """
    GLSL equivalent:
        layout(location=0) in  vec3 inPos;
        layout(location=1) in  vec3 inNormal;
        layout(location=0) out vec3 fragNormal;
        layout(push_constant) uniform PC { mat4 mvp; } pc;
        out gl_PerVertex { vec4 gl_Position; };
        void main() {
            gl_Position = pc.mvp * vec4(inPos, 1.0);
            fragNormal  = inNormal;
        }
    """
    s = Spirv()

    # ── IDs ───────────────────────────────────────────────────────────────────
    ID_VOID         = s.new()   # 1
    ID_FN_VT        = s.new()   # 2  TypeFunction(void)
    ID_FLOAT        = s.new()   # 3
    ID_VEC3         = s.new()   # 4
    ID_VEC4         = s.new()   # 5
    ID_MAT4         = s.new()   # 6  TypeMatrix(vec4, 4)
    ID_INT          = s.new()   # 7  TypeInt(32, signed)
    ID_PV_STRUCT    = s.new()   # 8  gl_PerVertex { vec4 Position }
    ID_PC_STRUCT    = s.new()   # 9  PC { mat4 mvp }
    ID_PTR_OUT_PV   = s.new()   # 10 TypePointer(Output, gl_PerVertex)
    ID_PTR_PC_S     = s.new()   # 11 TypePointer(PushConstant, PC_struct)
    ID_PTR_IN_V3    = s.new()   # 12 TypePointer(Input, vec3)
    ID_PTR_OUT_V3   = s.new()   # 13 TypePointer(Output, vec3)
    ID_PTR_OUT_V4   = s.new()   # 14 TypePointer(Output, vec4) for access chain
    ID_PTR_PC_M4    = s.new()   # 15 TypePointer(PushConstant, mat4)
    ID_GL_POS_BLOCK = s.new()   # 16 var gl_pos_block Output
    ID_PC_VAR       = s.new()   # 17 var pc PushConstant
    ID_IN_POS       = s.new()   # 18 var inPos Input
    ID_IN_NRM       = s.new()   # 19 var inNormal Input
    ID_OUT_NRM      = s.new()   # 20 var fragNormal Output
    ID_MAIN         = s.new()   # 21 function
    ID_ENTRY_LBL    = s.new()   # 22 label
    ID_CI0          = s.new()   # 23 const int 0
    ID_CF1          = s.new()   # 24 const float 1.0
    # Function body temporaries
    ID_LV3          = s.new()   # 25  loaded position vec3
    ID_PX           = s.new()   # 26
    ID_PY           = s.new()   # 27
    ID_PZ           = s.new()   # 28
    ID_POS4         = s.new()   # 29  vec4(px,py,pz,1)
    ID_P_MVP        = s.new()   # 30  AccessChain ptr to mvp
    ID_MVP          = s.new()   # 31  loaded mat4
    ID_RPOS         = s.new()   # 32  result position
    ID_P_GPOS       = s.new()   # 33  AccessChain ptr to gl_Position
    ID_LNRM         = s.new()   # 34  loaded normal

    # ── Section 1: Capabilities ───────────────────────────────────────────────
    s.emit(OP_CAPABILITY, CAP_SHADER)

    # ── Section 4: Memory model ───────────────────────────────────────────────
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)

    # ── Section 5: Entry point ────────────────────────────────────────────────
    s.entry_point(EXM_VERTEX, ID_MAIN, "main",
                  ID_IN_POS, ID_IN_NRM, ID_GL_POS_BLOCK, ID_OUT_NRM)

    # ── Section 8: Decorations ────────────────────────────────────────────────
    s.emit(OP_DECORATE, ID_IN_POS,  DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_IN_NRM,  DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_OUT_NRM, DEC_LOCATION, 0)
    # gl_PerVertex block
    s.emit(OP_DECORATE,        ID_PV_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PV_STRUCT, 0, DEC_BUILT_IN, BI_POSITION)
    # Push-constant block (layout std430)
    s.emit(OP_DECORATE,        ID_PC_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_COL_MAJOR)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_MATRIX_STRIDE, 16)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_OFFSET, 0)

    # ── Section 9: Type + constant + global variable declarations ─────────────
    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,   ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_MATRIX,   ID_MAT4, ID_VEC4, 4)   # mat4 = 4 columns of vec4
    s.emit(OP_TYPE_INT,      ID_INT, 32, 1)           # signed int

    s.emit(OP_TYPE_STRUCT,   ID_PV_STRUCT, ID_VEC4)  # gl_PerVertex { vec4 }
    s.emit(OP_TYPE_STRUCT,   ID_PC_STRUCT, ID_MAT4)  # PC { mat4 mvp }

    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_PV, SC_OUTPUT,       ID_PV_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_PC_S,   SC_PUSH_CONSTANT, ID_PC_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V3,  SC_INPUT,        ID_VEC3)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V3, SC_OUTPUT,       ID_VEC3)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V4, SC_OUTPUT,       ID_VEC4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_PC_M4,  SC_PUSH_CONSTANT, ID_MAT4)

    s.emit(OP_VARIABLE, ID_PTR_OUT_PV, ID_GL_POS_BLOCK, SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_PC_S,   ID_PC_VAR,        SC_PUSH_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,  ID_IN_POS,        SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,  ID_IN_NRM,        SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V3, ID_OUT_NRM,       SC_OUTPUT)

    s.emit(OP_CONSTANT, ID_INT,   ID_CI0, 0)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CF1, f2w(1.0))

    # ── Section 10: Function ──────────────────────────────────────────────────
    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)

    # Load position, build vec4(pos, 1.0)
    s.emit(OP_LOAD,               ID_VEC3,  ID_LV3,  ID_IN_POS)
    s.emit(OP_COMPOSITE_EXTRACT,  ID_FLOAT, ID_PX,   ID_LV3, 0)
    s.emit(OP_COMPOSITE_EXTRACT,  ID_FLOAT, ID_PY,   ID_LV3, 1)
    s.emit(OP_COMPOSITE_EXTRACT,  ID_FLOAT, ID_PZ,   ID_LV3, 2)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_POS4, ID_PX, ID_PY, ID_PZ, ID_CF1)

    # Load MVP and transform
    s.emit(OP_ACCESS_CHAIN,        ID_PTR_PC_M4, ID_P_MVP,  ID_PC_VAR, ID_CI0)
    s.emit(OP_LOAD,                ID_MAT4,      ID_MVP,    ID_P_MVP)
    s.emit(OP_MATRIX_TIMES_VECTOR, ID_VEC4,      ID_RPOS,   ID_MVP,    ID_POS4)

    # Write gl_Position
    s.emit(OP_ACCESS_CHAIN, ID_PTR_OUT_V4, ID_P_GPOS, ID_GL_POS_BLOCK, ID_CI0)
    s.emit(OP_STORE, ID_P_GPOS, ID_RPOS)

    # Pass-through normal
    s.emit(OP_LOAD,  ID_VEC3, ID_LNRM, ID_IN_NRM)
    s.emit(OP_STORE, ID_OUT_NRM, ID_LNRM)

    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)

    return s.build()


# ── Fragment shader ────────────────────────────────────────────────────────────
def build_fragment():
    """
    GLSL equivalent (no normalize/max extensions needed):
        layout(location=0) in  vec3 fragNormal;
        layout(location=0) out vec4 outColor;
        void main() {
            vec3  L    = vec3(0.57735, 0.57735, 0.57735);  // normalize(1,1,1)
            float diff = dot(fragNormal, L);
            diff       = clamp(diff * 0.45 + 0.55, 0.0, 1.0);  // approx
            outColor   = vec4(diff*0.60, diff*0.72, diff*0.90, 1.0);
        }
    Note: no OpClamp used here; dark faces get naturally darker ambient colour.
    """
    s = Spirv()

    # ── IDs ───────────────────────────────────────────────────────────────────
    ID_VOID       = s.new()   # 1
    ID_FN_VT      = s.new()   # 2
    ID_FLOAT      = s.new()   # 3
    ID_VEC3       = s.new()   # 4
    ID_VEC4       = s.new()   # 5
    ID_PTR_OUT_V4 = s.new()   # 6 TypePointer(Output, vec4)
    ID_PTR_IN_V3  = s.new()   # 7 TypePointer(Input, vec3)
    ID_FRAG_NRM   = s.new()   # 8 fragNormal Input var
    ID_OUT_COL    = s.new()   # 9 outColor Output var
    ID_MAIN       = s.new()   # 10
    ID_ENTRY_LBL  = s.new()   # 11
    ID_LC         = s.new()   # 12 const 0.57735 (all 3 components)
    ID_LIGHT      = s.new()   # 13 const vec3 light
    ID_C045       = s.new()   # 14 const 0.45
    ID_C055       = s.new()   # 15 const 0.55
    ID_C060       = s.new()   # 16 const 0.60
    ID_C072       = s.new()   # 17 const 0.72
    ID_C090       = s.new()   # 18 const 0.90
    ID_C1         = s.new()   # 19 const 1.0
    # runtime
    ID_LNRM       = s.new()   # 20 loaded fragNormal
    ID_DOT        = s.new()   # 21 dot(fragNormal, light)
    ID_SCALED     = s.new()   # 22 dot * 0.45
    ID_BIASED     = s.new()   # 23 scaled + 0.55
    ID_R          = s.new()   # 24 biased * 0.60
    ID_G          = s.new()   # 25 biased * 0.72
    ID_B          = s.new()   # 26 biased * 0.90
    ID_COLOR      = s.new()   # 27 vec4 composite

    # ── Capability ────────────────────────────────────────────────────────────
    s.emit(OP_CAPABILITY, CAP_SHADER)

    # ── Memory model ──────────────────────────────────────────────────────────
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)

    # ── Entry point ───────────────────────────────────────────────────────────
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main", ID_FRAG_NRM, ID_OUT_COL)

    # ── Execution mode ────────────────────────────────────────────────────────
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    # ── Decorations ───────────────────────────────────────────────────────────
    s.emit(OP_DECORATE, ID_FRAG_NRM, DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_OUT_COL,  DEC_LOCATION, 0)

    # ── Type + constant + global variable declarations ─────────────────────────
    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,   ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V4, SC_OUTPUT, ID_VEC4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V3,  SC_INPUT,  ID_VEC3)

    s.emit(OP_VARIABLE, ID_PTR_IN_V3,  ID_FRAG_NRM, SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4, ID_OUT_COL,  SC_OUTPUT)

    s.emit(OP_CONSTANT,          ID_FLOAT, ID_LC,   f2w(0.57735))
    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC3, ID_LIGHT, ID_LC, ID_LC, ID_LC)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C045, f2w(0.45))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C055, f2w(0.55))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C060, f2w(0.60))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C072, f2w(0.72))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C090, f2w(0.90))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C1,   f2w(1.0))

    # ── Function ──────────────────────────────────────────────────────────────
    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)

    s.emit(OP_LOAD,   ID_VEC3,  ID_LNRM,   ID_FRAG_NRM)
    s.emit(OP_DOT,    ID_FLOAT, ID_DOT,    ID_LNRM, ID_LIGHT)
    s.emit(OP_F_MUL,  ID_FLOAT, ID_SCALED, ID_DOT,    ID_C045)
    s.emit(OP_F_ADD,  ID_FLOAT, ID_BIASED, ID_SCALED, ID_C055)
    s.emit(OP_F_MUL,  ID_FLOAT, ID_R,      ID_BIASED, ID_C060)
    s.emit(OP_F_MUL,  ID_FLOAT, ID_G,      ID_BIASED, ID_C072)
    s.emit(OP_F_MUL,  ID_FLOAT, ID_B,      ID_BIASED, ID_C090)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_COLOR,
           ID_R, ID_G, ID_B, ID_C1)
    s.emit(OP_STORE, ID_OUT_COL, ID_COLOR)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)

    return s.build()


# ── C++ header output ──────────────────────────────────────────────────────────
def to_cpp_header(array_name, data):
    words = struct.unpack(f'<{len(data)//4}I', data)
    lines = [
        '#pragma once',
        '// AUTO-GENERATED by tools/gen_shaders.py -- DO NOT EDIT',
        '#include <cstdint>',
        f'static const uint32_t {array_name}[] = {{',
    ]
    for i in range(0, len(words), 8):
        chunk = ', '.join(f'0x{w:08X}' for w in words[i:i+8])
        lines.append(f'    {chunk},')
    lines.append('};')
    lines.append(f'static const uint32_t {array_name}_size = sizeof({array_name});')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    root = pathlib.Path(__file__).parent.parent
    out  = root / 'src' / 'render' / 'shaders'
    out.mkdir(parents=True, exist_ok=True)

    vert_spv = build_vertex()
    frag_spv = build_fragment()

    (out / 'chunk_vert_spv.h').write_text(to_cpp_header('k_chunk_vert_spv', vert_spv))
    (out / 'chunk_frag_spv.h').write_text(to_cpp_header('k_chunk_frag_spv', frag_spv))

    print(f'chunk_vert_spv.h : {len(vert_spv)} bytes  ({len(vert_spv)//4} words)')
    print(f'chunk_frag_spv.h : {len(frag_spv)} bytes  ({len(frag_spv)//4} words)')
    print('Done.')
