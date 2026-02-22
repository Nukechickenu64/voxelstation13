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
SC_UNIFORM_CONSTANT = 0  # combined image sampler (descriptor)
SC_INPUT            = 1
SC_UNIFORM          = 2   # UBO — used by SDL_PushGPUVertexUniformData
SC_OUTPUT           = 3
SC_PUSH_CONSTANT    = 9   # NOT used by SDL3 GPU — kept for reference only

# Decoration
DEC_BLOCK          = 2
DEC_COL_MAJOR      = 5
DEC_MATRIX_STRIDE  = 7
DEC_BUILT_IN       = 11
DEC_LOCATION       = 30
DEC_BINDING        = 33  # Vulkan descriptor binding number
DEC_DESCRIPTOR_SET = 34  # Vulkan descriptor set number
DEC_OFFSET         = 35

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
OP_TYPE_IMAGE          = 25
OP_TYPE_SAMPLED_IMAGE  = 27
OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87


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
        layout(location=0) in  vec3  inPos;
        layout(location=1) in  vec3  inNormal;
        layout(location=2) in  vec2  inUV;
        layout(location=3) in  float inTexIndex;
        layout(location=0) out vec3  fragNormal;
        layout(location=1) out vec2  fragUV;
        layout(location=2) out float fragTexIndex;
        layout(set=1, binding=0) uniform UBO { mat4 mvp; } ubo;
        out gl_PerVertex { vec4 gl_Position; };
        void main() {
            gl_Position  = ubo.mvp * vec4(inPos, 1.0);
            fragNormal   = inNormal;
            fragUV       = inUV;
            fragTexIndex = inTexIndex;
        }
    """
    s = Spirv()

    # ── IDs ───────────────────────────────────────────────────────────────────
    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC3         = s.new()
    ID_VEC4         = s.new()
    ID_MAT4         = s.new()
    ID_INT          = s.new()
    ID_PV_STRUCT    = s.new()
    ID_PC_STRUCT    = s.new()
    ID_PTR_OUT_PV   = s.new()
    ID_PTR_PC_S     = s.new()
    ID_PTR_IN_V3    = s.new()
    ID_PTR_OUT_V3   = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_PTR_PC_M4    = s.new()
    ID_GL_POS_BLOCK = s.new()
    ID_PC_VAR       = s.new()
    ID_IN_POS       = s.new()
    ID_IN_NRM       = s.new()
    ID_OUT_NRM      = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    ID_CI0          = s.new()
    ID_CF1          = s.new()
    ID_LV3          = s.new()
    ID_PX           = s.new()
    ID_PY           = s.new()
    ID_PZ           = s.new()
    ID_POS4         = s.new()
    ID_P_MVP        = s.new()
    ID_MVP          = s.new()
    ID_RPOS         = s.new()
    ID_P_GPOS       = s.new()
    ID_LNRM         = s.new()
    ID_VEC2         = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_OUT_V2   = s.new()
    ID_IN_UV        = s.new()
    ID_OUT_UV       = s.new()
    ID_LUV          = s.new()
    # texIndex support
    ID_PTR_IN_F     = s.new()
    ID_PTR_OUT_F    = s.new()
    ID_IN_TEXIDX    = s.new()
    ID_OUT_TEXIDX   = s.new()
    ID_LTIDX        = s.new()

    # ── Capabilities ─────────────────────────────────────────────────────────
    s.emit(OP_CAPABILITY, CAP_SHADER)

    # ── Memory model ──────────────────────────────────────────────────────────
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)

    # ── Entry point ───────────────────────────────────────────────────────────
    s.entry_point(EXM_VERTEX, ID_MAIN, "main",
                  ID_IN_POS, ID_IN_NRM, ID_IN_UV, ID_IN_TEXIDX,
                  ID_GL_POS_BLOCK, ID_OUT_NRM, ID_OUT_UV, ID_OUT_TEXIDX)

    # ── Decorations ───────────────────────────────────────────────────────────
    s.emit(OP_DECORATE, ID_IN_POS,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_IN_NRM,     DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_IN_UV,      DEC_LOCATION, 2)
    s.emit(OP_DECORATE, ID_IN_TEXIDX,  DEC_LOCATION, 3)
    s.emit(OP_DECORATE, ID_OUT_NRM,    DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_OUT_UV,     DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_OUT_TEXIDX, DEC_LOCATION, 2)
    # gl_PerVertex block
    s.emit(OP_DECORATE,        ID_PV_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PV_STRUCT, 0, DEC_BUILT_IN, BI_POSITION)
    # UBO block for MVP (SDL3 GPU vertex uniform slot 0 → set=1, binding=0)
    s.emit(OP_DECORATE,        ID_PC_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_COL_MAJOR)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_MATRIX_STRIDE, 16)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_OFFSET, 0)
    s.emit(OP_DECORATE, ID_PC_VAR, DEC_DESCRIPTOR_SET, 1)
    s.emit(OP_DECORATE, ID_PC_VAR, DEC_BINDING,        0)

    # ── Types + constants + global variables ──────────────────────────────────
    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,   ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_MATRIX,   ID_MAT4, ID_VEC4, 4)
    s.emit(OP_TYPE_INT,      ID_INT, 32, 1)
    s.emit(OP_TYPE_VECTOR,   ID_VEC2, ID_FLOAT, 2)

    s.emit(OP_TYPE_STRUCT,   ID_PV_STRUCT, ID_VEC4)
    s.emit(OP_TYPE_STRUCT,   ID_PC_STRUCT, ID_MAT4)

    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_PV, SC_OUTPUT,  ID_PV_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_PC_S,   SC_UNIFORM, ID_PC_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V3,  SC_INPUT,   ID_VEC3)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V3, SC_OUTPUT,  ID_VEC3)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V4, SC_OUTPUT,  ID_VEC4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_PC_M4,  SC_UNIFORM, ID_MAT4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V2,  SC_INPUT,   ID_VEC2)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V2, SC_OUTPUT,  ID_VEC2)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_F,   SC_INPUT,   ID_FLOAT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_F,  SC_OUTPUT,  ID_FLOAT)

    s.emit(OP_VARIABLE, ID_PTR_OUT_PV, ID_GL_POS_BLOCK, SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_PC_S,   ID_PC_VAR,        SC_UNIFORM)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,  ID_IN_POS,        SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,  ID_IN_NRM,        SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V3, ID_OUT_NRM,       SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,  ID_IN_UV,         SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V2, ID_OUT_UV,        SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_F,   ID_IN_TEXIDX,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_F,  ID_OUT_TEXIDX,    SC_OUTPUT)

    s.emit(OP_CONSTANT, ID_INT,   ID_CI0, 0)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CF1, f2w(1.0))

    # ── Function ──────────────────────────────────────────────────────────────
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

    # Pass-through UV
    s.emit(OP_LOAD,  ID_VEC2, ID_LUV, ID_IN_UV)
    s.emit(OP_STORE, ID_OUT_UV, ID_LUV)

    # Pass-through texIndex
    s.emit(OP_LOAD,  ID_FLOAT, ID_LTIDX, ID_IN_TEXIDX)
    s.emit(OP_STORE, ID_OUT_TEXIDX, ID_LTIDX)

    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)

    return s.build()


# ── Fragment shader ────────────────────────────────────────────────────────────
def build_fragment():
    """
    GLSL equivalent:
        layout(set=0, binding=0) uniform sampler2DArray tex;
        layout(location=0) in vec3  fragNormal;
        layout(location=1) in vec2  fragUV;
        layout(location=2) in float fragTexIndex;
        layout(location=0) out vec4 outColor;
        void main() {
            vec3  L    = vec3(0.57735, 0.57735, 0.57735);
            float diff = dot(fragNormal, L) * 0.45 + 0.55;
            vec4  tc   = texture(tex, vec3(fragUV, fragTexIndex));
            outColor   = vec4(tc.rgb * diff, tc.a);
        }
    """
    s = Spirv()

    # ── IDs ───────────────────────────────────────────────────────────────────
    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC3         = s.new()
    ID_VEC4         = s.new()
    ID_IMAGE        = s.new()   # TypeImage(float, 2D, 0, arrayed=1, ms=0, sampled=1, Unknown)
    ID_SAMP_IMG     = s.new()   # TypeSampledImage
    ID_PTR_UC_SIMG  = s.new()   # TypePointer(UniformConstant, SampledImage)
    ID_PTR_IN_V3    = s.new()   # TypePointer(Input, vec3)
    ID_PTR_IN_V2    = s.new()   # TypePointer(Input, vec2)
    ID_PTR_IN_F     = s.new()   # TypePointer(Input, float)
    ID_PTR_OUT_V4   = s.new()   # TypePointer(Output, vec4)
    ID_SAMPLER_VAR  = s.new()   # var tex  UniformConstant (set=0, binding=0)
    ID_FRAG_NRM     = s.new()   # fragNormal  Input  location=0
    ID_FRAG_UV      = s.new()   # fragUV      Input  location=1
    ID_FRAG_TEXIDX  = s.new()   # fragTexIndex Input location=2
    ID_OUT_COL      = s.new()   # outColor    Output location=0
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    # Constants
    ID_LC           = s.new()   # 0.57735
    ID_LIGHT        = s.new()   # vec3(lc, lc, lc)
    ID_C045         = s.new()
    ID_C055         = s.new()
    ID_C1           = s.new()
    # Runtime
    ID_LNRM         = s.new()
    ID_DOT          = s.new()
    ID_SCALED       = s.new()
    ID_BIASED       = s.new()
    ID_LUV          = s.new()
    ID_LTIDX        = s.new()
    ID_UVX          = s.new()
    ID_UVY          = s.new()
    ID_UVW          = s.new()   # vec3(uvx, uvy, texidx)
    ID_SIMG_LOADED  = s.new()
    ID_TC           = s.new()   # sampled vec4
    ID_TC_R         = s.new()
    ID_TC_G         = s.new()
    ID_TC_B         = s.new()
    ID_TC_A         = s.new()
    ID_OUT_R        = s.new()
    ID_OUT_G        = s.new()
    ID_OUT_B        = s.new()
    ID_OUT_COLOR    = s.new()

    # ── Capabilities ─────────────────────────────────────────────────────────
    s.emit(OP_CAPABILITY, CAP_SHADER)

    # ── Memory model ──────────────────────────────────────────────────────────
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)

    # ── Entry point (Input/Output vars only, not UniformConstant) ────────────
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main",
                  ID_FRAG_NRM, ID_FRAG_UV, ID_FRAG_TEXIDX, ID_OUT_COL)

    # ── Execution mode ────────────────────────────────────────────────────────
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    # ── Decorations ───────────────────────────────────────────────────────────
    s.emit(OP_DECORATE, ID_FRAG_NRM,    DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_FRAG_UV,     DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_FRAG_TEXIDX, DEC_LOCATION, 2)
    s.emit(OP_DECORATE, ID_OUT_COL,     DEC_LOCATION, 0)
    # Sampler descriptor (fragment set=2, binding=0 — SDL3 GPU SPIRV layout)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_BINDING,        0)

    # ── Types + constants + global variables ──────────────────────────────────
    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,   ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,   ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4, ID_FLOAT, 4)
    # OpTypeImage: sampled_type Dim Depth Arrayed MS Sampled Format
    s.emit(OP_TYPE_IMAGE,         ID_IMAGE,    ID_FLOAT, 1, 0, 1, 0, 1, 0)
    s.emit(OP_TYPE_SAMPLED_IMAGE, ID_SAMP_IMG, ID_IMAGE)
    s.emit(OP_TYPE_POINTER, ID_PTR_UC_SIMG, SC_UNIFORM_CONSTANT, ID_SAMP_IMG)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V3,   SC_INPUT,             ID_VEC3)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,   SC_INPUT,             ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_F,    SC_INPUT,             ID_FLOAT)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4,  SC_OUTPUT,            ID_VEC4)

    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_SAMPLER_VAR, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,   ID_FRAG_NRM,    SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_FRAG_UV,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_F,    ID_FRAG_TEXIDX, SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4,  ID_OUT_COL,     SC_OUTPUT)

    s.emit(OP_CONSTANT,           ID_FLOAT, ID_LC,   f2w(0.57735))
    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC3,  ID_LIGHT, ID_LC, ID_LC, ID_LC)
    s.emit(OP_CONSTANT,           ID_FLOAT, ID_C045, f2w(0.45))
    s.emit(OP_CONSTANT,           ID_FLOAT, ID_C055, f2w(0.55))
    s.emit(OP_CONSTANT,           ID_FLOAT, ID_C1,   f2w(1.0))

    # ── Function ──────────────────────────────────────────────────────────────
    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)

    # Diffuse: dot(fragNormal, L) * 0.45 + 0.55
    s.emit(OP_LOAD,  ID_VEC3,  ID_LNRM,   ID_FRAG_NRM)
    s.emit(OP_DOT,   ID_FLOAT, ID_DOT,    ID_LNRM, ID_LIGHT)
    s.emit(OP_F_MUL, ID_FLOAT, ID_SCALED, ID_DOT,    ID_C045)
    s.emit(OP_F_ADD, ID_FLOAT, ID_BIASED, ID_SCALED, ID_C055)

    # Build vec3 texture coordinate: vec3(fragUV.x, fragUV.y, fragTexIndex)
    s.emit(OP_LOAD,              ID_VEC2,  ID_LUV,   ID_FRAG_UV)
    s.emit(OP_LOAD,              ID_FLOAT, ID_LTIDX, ID_FRAG_TEXIDX)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_UVX,   ID_LUV, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_UVY,   ID_LUV, 1)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC3, ID_UVW,  ID_UVX, ID_UVY, ID_LTIDX)

    # Sample the texture array
    s.emit(OP_LOAD, ID_SAMP_IMG, ID_SIMG_LOADED, ID_SAMPLER_VAR)
    s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, ID_TC, ID_SIMG_LOADED, ID_UVW)

    # Extract RGBA components
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_R, ID_TC, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_G, ID_TC, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_B, ID_TC, 2)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_A, ID_TC, 3)

    # Multiply RGB by diffuse factor
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_R, ID_TC_R, ID_BIASED)
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_G, ID_TC_G, ID_BIASED)
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_B, ID_TC_B, ID_BIASED)

    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_OUT_COLOR,
           ID_OUT_R, ID_OUT_G, ID_OUT_B, ID_TC_A)
    s.emit(OP_STORE, ID_OUT_COL, ID_OUT_COLOR)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)

    return s.build()


# ── Highlight vertex shader ──────────────────────────────────────────────────────
def build_highlight_vertex():
    """
    Minimal vertex shader for face highlight:
        layout(location=0) in vec3 inPos;
        layout(push_constant) uniform PC { mat4 mvp; } pc;
        out gl_PerVertex { vec4 gl_Position; };
        void main() { gl_Position = pc.mvp * vec4(inPos, 1.0); }
    """
    s = Spirv()
    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC3         = s.new()
    ID_VEC4         = s.new()
    ID_MAT4         = s.new()
    ID_INT          = s.new()
    ID_PV_STRUCT    = s.new()
    ID_PC_STRUCT    = s.new()
    ID_PTR_OUT_PV   = s.new()
    ID_PTR_PC_S     = s.new()
    ID_PTR_IN_V3    = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_PTR_PC_M4    = s.new()
    ID_GL_POS_BLOCK = s.new()
    ID_PC_VAR       = s.new()
    ID_IN_POS       = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    ID_CI0          = s.new()
    ID_CF1          = s.new()
    ID_LV3          = s.new()
    ID_PX           = s.new()
    ID_PY           = s.new()
    ID_PZ           = s.new()
    ID_POS4         = s.new()
    ID_P_MVP        = s.new()
    ID_MVP          = s.new()
    ID_RPOS         = s.new()
    ID_P_GPOS       = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_VERTEX, ID_MAIN, "main", ID_IN_POS, ID_GL_POS_BLOCK)

    s.emit(OP_DECORATE, ID_IN_POS, DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_PV_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PV_STRUCT, 0, DEC_BUILT_IN, BI_POSITION)
    s.emit(OP_DECORATE, ID_PC_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_COL_MAJOR)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_MATRIX_STRIDE, 16)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_OFFSET, 0)
    s.emit(OP_DECORATE, ID_PC_VAR, DEC_DESCRIPTOR_SET, 1)
    s.emit(OP_DECORATE, ID_PC_VAR, DEC_BINDING,        0)

    s.emit(OP_TYPE_VOID, ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT, ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR, ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR, ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_MATRIX, ID_MAT4, ID_VEC4, 4)
    s.emit(OP_TYPE_INT, ID_INT, 32, 1)
    s.emit(OP_TYPE_STRUCT, ID_PV_STRUCT, ID_VEC4)
    s.emit(OP_TYPE_STRUCT, ID_PC_STRUCT, ID_MAT4)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_PV, SC_OUTPUT,  ID_PV_STRUCT)
    s.emit(OP_TYPE_POINTER, ID_PTR_PC_S,   SC_UNIFORM, ID_PC_STRUCT)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V3,  SC_INPUT,   ID_VEC3)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4, SC_OUTPUT,  ID_VEC4)
    s.emit(OP_TYPE_POINTER, ID_PTR_PC_M4,  SC_UNIFORM, ID_MAT4)
    s.emit(OP_VARIABLE, ID_PTR_OUT_PV, ID_GL_POS_BLOCK, SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_PC_S,   ID_PC_VAR,        SC_UNIFORM)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,  ID_IN_POS,        SC_INPUT)
    s.emit(OP_CONSTANT, ID_INT,   ID_CI0, 0)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CF1, f2w(1.0))

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)
    s.emit(OP_LOAD,               ID_VEC3,  ID_LV3,  ID_IN_POS)
    s.emit(OP_COMPOSITE_EXTRACT,  ID_FLOAT, ID_PX,   ID_LV3, 0)
    s.emit(OP_COMPOSITE_EXTRACT,  ID_FLOAT, ID_PY,   ID_LV3, 1)
    s.emit(OP_COMPOSITE_EXTRACT,  ID_FLOAT, ID_PZ,   ID_LV3, 2)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_POS4, ID_PX, ID_PY, ID_PZ, ID_CF1)
    s.emit(OP_ACCESS_CHAIN,        ID_PTR_PC_M4, ID_P_MVP,  ID_PC_VAR, ID_CI0)
    s.emit(OP_LOAD,                ID_MAT4,      ID_MVP,    ID_P_MVP)
    s.emit(OP_MATRIX_TIMES_VECTOR, ID_VEC4,      ID_RPOS,   ID_MVP,    ID_POS4)
    s.emit(OP_ACCESS_CHAIN, ID_PTR_OUT_V4, ID_P_GPOS, ID_GL_POS_BLOCK, ID_CI0)
    s.emit(OP_STORE, ID_P_GPOS, ID_RPOS)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


# ── Highlight fragment shader ──────────────────────────────────────────────────
def build_highlight_fragment():
    """
    Fixed yellow-orange color for face selection highlight:
        layout(location=0) out vec4 outColor;
        void main() { outColor = vec4(1.0, 0.78, 0.1, 0.85); }
    """
    s = Spirv()
    ID_VOID       = s.new()
    ID_FN_VT      = s.new()
    ID_FLOAT      = s.new()
    ID_VEC4       = s.new()
    ID_PTR_OUT_V4 = s.new()
    ID_OUT_COL    = s.new()
    ID_MAIN       = s.new()
    ID_ENTRY_LBL  = s.new()
    ID_CR         = s.new()
    ID_CG         = s.new()
    ID_CB         = s.new()
    ID_CA         = s.new()
    ID_COLOR      = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main", ID_OUT_COL)
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)
    s.emit(OP_DECORATE, ID_OUT_COL, DEC_LOCATION, 0)

    s.emit(OP_TYPE_VOID, ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT, ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR, ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4, SC_OUTPUT, ID_VEC4)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4, ID_OUT_COL, SC_OUTPUT)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CR, f2w(1.00))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CG, f2w(0.78))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CB, f2w(0.10))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CA, f2w(0.85))
    # OP_CONSTANT_COMPOSITE must live in the global section, before any function.
    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC4, ID_COLOR, ID_CR, ID_CG, ID_CB, ID_CA)

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)
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

    vert_spv    = build_vertex()
    frag_spv    = build_fragment()
    hl_vert_spv = build_highlight_vertex()
    hl_frag_spv = build_highlight_fragment()

    (out / 'chunk_vert_spv.h').write_text(to_cpp_header('k_chunk_vert_spv', vert_spv))
    (out / 'chunk_frag_spv.h').write_text(to_cpp_header('k_chunk_frag_spv', frag_spv))
    (out / 'highlight_vert_spv.h').write_text(to_cpp_header('k_highlight_vert_spv', hl_vert_spv))
    (out / 'highlight_frag_spv.h').write_text(to_cpp_header('k_highlight_frag_spv', hl_frag_spv))

    print(f'chunk_vert_spv.h    : {len(vert_spv)} bytes  ({len(vert_spv)//4} words)')
    print(f'chunk_frag_spv.h    : {len(frag_spv)} bytes  ({len(frag_spv)//4} words)')
    print(f'highlight_vert_spv.h: {len(hl_vert_spv)} bytes  ({len(hl_vert_spv)//4} words)')
    print(f'highlight_frag_spv.h: {len(hl_frag_spv)} bytes  ({len(hl_frag_spv)//4} words)')
    print('Done.')
