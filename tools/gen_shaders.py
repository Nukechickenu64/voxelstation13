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
DEC_NO_PERSPECTIVE = 10
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
OP_F_SUB               = 131
OP_FUNCTION            = 54
OP_FUNCTION_END        = 56
OP_LABEL               = 248
OP_RETURN              = 253
OP_TYPE_BOOL           = 20
OP_TYPE_IMAGE          = 25
OP_TYPE_SAMPLED_IMAGE  = 27
OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87
OP_F_ORD_GREATER_THAN  = 186
OP_F_ORD_LESS_THAN     = 184
OP_SELECT              = 169
OP_SELECTION_MERGE     = 247
OP_BRANCH              = 249
OP_BRANCH_CONDITIONAL  = 250
OP_KILL                = 252
OP_EXT_INST            = 12   # OpExtInst (for GLSL.std.450 extensions)
OP_F_DIV               = 136  # OpFDiv


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
        layout(location=4) in  vec3  inLightColor;   // RGB light color × brightness
        layout(location=5) in  float inAO;           // ambient occlusion 0-1
        layout(location=0) out vec3  fragNormal;
        layout(location=1) out vec2  fragUV;
        layout(location=2) out float fragTexIndex;
        layout(location=3) out vec3  fragLightColor; // pass-through
        layout(location=4) out float fragAO;         // pass-through
        layout(set=1, binding=0) uniform UBO { mat4 mvp; } ubo;
        out gl_PerVertex { vec4 gl_Position; };
        void main() {
            gl_Position    = ubo.mvp * vec4(inPos, 1.0);
            fragNormal     = inNormal;
            fragUV         = inUV;
            fragTexIndex   = inTexIndex;
            fragLightColor = inLightColor;
            fragAO         = inAO;
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
    # Light color pass-through (location 4, vec3)
    ID_IN_LIGHT     = s.new()   # location 4: vec3 inLightColor
    ID_OUT_LIGHT    = s.new()   # location 3 out: vec3 fragLightColor
    ID_LLIGHT       = s.new()   # loaded vec3
    # AO pass-through (location 5)
    ID_IN_AO        = s.new()
    ID_OUT_AO       = s.new()
    ID_LAO          = s.new()
    # PSX wobble
    ID_GLSL_EXT     = s.new()   # GLSL.std.450 extension handle
    ID_BOOL         = s.new()   # bool type
    ID_PTR_PC_V4    = s.new()   # pointer to vec4 in uniform (for psx_opts)
    ID_CI1          = s.new()   # int constant 1
    ID_C05_PSX      = s.new()   # float 0.5 (selection/rounding threshold)
    ID_CHIGH        = s.new()   # float 1e9 (high snap = disabled: imperceptible)
    ID_PSX_P_OPTS   = s.new()   # access chain → psx_opts vec4
    ID_PSX_OPTS     = s.new()   # loaded psx_opts vec4
    ID_PSX_SNAP_RAW = s.new()   # psx_opts.x = raw snap resolution
    ID_PSX_DO_PSX   = s.new()   # bool: snap_raw > 0.5
    ID_PSX_SNAP     = s.new()   # selected actual snap resolution
    ID_RPOS_X       = s.new()   # clip-space x component
    ID_RPOS_Y       = s.new()   # clip-space y component
    ID_RPOS_Z       = s.new()   # clip-space z component
    ID_RPOS_W       = s.new()   # clip-space w component
    ID_NX           = s.new()   # x / w (NDC x)
    ID_NY           = s.new()   # y / w (NDC y)
    ID_SN_NX        = s.new()   # nx * snap_res
    ID_SN_NY        = s.new()   # ny * snap_res
    ID_SN_NX_05     = s.new()   # + 0.5
    ID_SN_NY_05     = s.new()   # + 0.5
    ID_FL_NX        = s.new()   # floor(...)
    ID_FL_NY        = s.new()
    ID_FL_NX_DS     = s.new()   # / snap_res
    ID_FL_NY_DS     = s.new()
    ID_SX           = s.new()   # snapped clip x
    ID_SY           = s.new()   # snapped clip y
    ID_YSHEAR       = s.new()   # psx_opts.y = y-shear amount
    ID_YSHEAR_TERM  = s.new()   # y_shear * clip_w
    ID_SY_SHEARED   = s.new()   # SY + YSHEAR_TERM (final clip y)
    ID_RPOS_FINAL    = s.new()   # final vec4 position
    # Affine texture mapping (NoPerspective outputs, locations 5 & 6)
    ID_OUT_AFF_UV     = s.new()   # location 5: NoPerspective vec2
    ID_OUT_AFF_TEXIDX = s.new()   # location 6: NoPerspective float

    # ── Capabilities ─────────────────────────────────────────────────────────
    s.emit(OP_CAPABILITY, CAP_SHADER)

    # ── Extension import (GLSL.std.450 for Floor) ─────────────────────────────
    OP_EXT_INST_IMPORT = 11
    ext_words = str_words("GLSL.std.450")
    wc = 1 + 1 + len(ext_words)
    s.words.append(wc << 16 | OP_EXT_INST_IMPORT)
    s.words.append(ID_GLSL_EXT)
    s.words.extend(ext_words)

    # ── Memory model ──────────────────────────────────────────────────────────
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)

    # ── Entry point ───────────────────────────────────────────────────────────
    s.entry_point(EXM_VERTEX, ID_MAIN, "main",
                  ID_IN_POS, ID_IN_NRM, ID_IN_UV, ID_IN_TEXIDX, ID_IN_LIGHT, ID_IN_AO,
                  ID_GL_POS_BLOCK, ID_OUT_NRM, ID_OUT_UV, ID_OUT_TEXIDX, ID_OUT_LIGHT, ID_OUT_AO,
                  ID_OUT_AFF_UV, ID_OUT_AFF_TEXIDX)

    # ── Decorations ───────────────────────────────────────────────────────────
    s.emit(OP_DECORATE, ID_IN_POS,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_IN_NRM,     DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_IN_UV,      DEC_LOCATION, 2)
    s.emit(OP_DECORATE, ID_IN_TEXIDX,  DEC_LOCATION, 3)
    s.emit(OP_DECORATE, ID_IN_LIGHT,   DEC_LOCATION, 4)
    s.emit(OP_DECORATE, ID_IN_AO,      DEC_LOCATION, 5)
    s.emit(OP_DECORATE, ID_OUT_NRM,    DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_OUT_UV,     DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_OUT_TEXIDX, DEC_LOCATION, 2)
    s.emit(OP_DECORATE, ID_OUT_LIGHT,  DEC_LOCATION, 3)
    s.emit(OP_DECORATE, ID_OUT_AO,       DEC_LOCATION, 4)
    # Affine UV outputs – NoPerspective = linear (screen-space) interpolation
    s.emit(OP_DECORATE, ID_OUT_AFF_UV,     DEC_LOCATION, 5)
    s.emit(OP_DECORATE, ID_OUT_AFF_UV,     DEC_NO_PERSPECTIVE)
    s.emit(OP_DECORATE, ID_OUT_AFF_TEXIDX, DEC_LOCATION, 6)
    s.emit(OP_DECORATE, ID_OUT_AFF_TEXIDX, DEC_NO_PERSPECTIVE)
    # gl_PerVertex block
    s.emit(OP_DECORATE,        ID_PV_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PV_STRUCT, 0, DEC_BUILT_IN, BI_POSITION)
    # UBO block for MVP + psx_opts (SDL3 GPU vertex uniform slot 0 → set=1, binding=0)
    # struct VertexUBO { mat4 mvp /*offset 0*/; vec4 psx_opts /*offset 64*/; }
    s.emit(OP_DECORATE,        ID_PC_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_COL_MAJOR)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_MATRIX_STRIDE, 16)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_OFFSET, 0)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 1, DEC_OFFSET, 64)  # psx_opts at byte 64
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
    s.emit(OP_TYPE_STRUCT,   ID_PC_STRUCT, ID_MAT4, ID_VEC4)  # { mat4 mvp; vec4 psx_opts; }
    s.emit(OP_TYPE_BOOL,     ID_BOOL)

    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_PV, SC_OUTPUT,  ID_PV_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_PC_S,   SC_UNIFORM, ID_PC_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V3,  SC_INPUT,   ID_VEC3)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V3, SC_OUTPUT,  ID_VEC3)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V4, SC_OUTPUT,  ID_VEC4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_PC_M4,  SC_UNIFORM, ID_MAT4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_PC_V4,  SC_UNIFORM, ID_VEC4)  # for psx_opts
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
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,  ID_IN_LIGHT,      SC_INPUT)   # vec3 light color
    s.emit(OP_VARIABLE, ID_PTR_OUT_V3, ID_OUT_LIGHT,     SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_F,    ID_IN_AO,          SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_F,   ID_OUT_AO,         SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V2,  ID_OUT_AFF_UV,     SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_F,   ID_OUT_AFF_TEXIDX, SC_OUTPUT)

    s.emit(OP_CONSTANT, ID_INT,   ID_CI0, 0)
    s.emit(OP_CONSTANT, ID_INT,   ID_CI1, 1)   # member index for psx_opts
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CF1, f2w(1.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C05_PSX, f2w(0.5))    # comparison threshold
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CHIGH,   f2w(1.0e9))  # "disabled" precision

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

    # ── PSX vertex wobble ─────────────────────────────────────────────────────
    # Load ubo.psx_opts (member 1, at offset 64) and extract snap_resolution (x)
    GLSL_FLOOR = 8
    s.emit(OP_ACCESS_CHAIN, ID_PTR_PC_V4, ID_PSX_P_OPTS, ID_PC_VAR, ID_CI1)
    s.emit(OP_LOAD,  ID_VEC4,  ID_PSX_OPTS,     ID_PSX_P_OPTS)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_PSX_SNAP_RAW, ID_PSX_OPTS, 0)
    # do_psx = snap_raw > 0.5  (0 = disabled)
    s.emit(OP_F_ORD_GREATER_THAN, ID_BOOL, ID_PSX_DO_PSX, ID_PSX_SNAP_RAW, ID_C05_PSX)
    # actual_snap = do_psx ? snap_raw : 1e9  (branchless; 1e9 = imperceptible grid)
    s.emit(OP_SELECT, ID_FLOAT, ID_PSX_SNAP, ID_PSX_DO_PSX, ID_PSX_SNAP_RAW, ID_CHIGH)
    # Extract clip-space components
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_RPOS_X, ID_RPOS, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_RPOS_Y, ID_RPOS, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_RPOS_Z, ID_RPOS, 2)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_RPOS_W, ID_RPOS, 3)
    # Perspective divide → NDC
    s.emit(OP_F_DIV, ID_FLOAT, ID_NX, ID_RPOS_X, ID_RPOS_W)
    s.emit(OP_F_DIV, ID_FLOAT, ID_NY, ID_RPOS_Y, ID_RPOS_W)
    # Snap: floor(ndc * snap + 0.5) / snap
    s.emit(OP_F_MUL, ID_FLOAT, ID_SN_NX,   ID_NX,     ID_PSX_SNAP)
    s.emit(OP_F_MUL, ID_FLOAT, ID_SN_NY,   ID_NY,     ID_PSX_SNAP)
    s.emit(OP_F_ADD, ID_FLOAT, ID_SN_NX_05, ID_SN_NX, ID_C05_PSX)
    s.emit(OP_F_ADD, ID_FLOAT, ID_SN_NY_05, ID_SN_NY, ID_C05_PSX)
    s.emit(OP_EXT_INST, ID_FLOAT, ID_FL_NX, ID_GLSL_EXT, GLSL_FLOOR, ID_SN_NX_05)
    s.emit(OP_EXT_INST, ID_FLOAT, ID_FL_NY, ID_GLSL_EXT, GLSL_FLOOR, ID_SN_NY_05)
    s.emit(OP_F_DIV, ID_FLOAT, ID_FL_NX_DS, ID_FL_NX, ID_PSX_SNAP)
    s.emit(OP_F_DIV, ID_FLOAT, ID_FL_NY_DS, ID_FL_NY, ID_PSX_SNAP)
    # Re-multiply by w to restore homogeneous clip coordinates
    s.emit(OP_F_MUL, ID_FLOAT, ID_SX, ID_FL_NX_DS, ID_RPOS_W)
    s.emit(OP_F_MUL, ID_FLOAT, ID_SY, ID_FL_NY_DS, ID_RPOS_W)
    # Y-shear: extract psx_opts.y and offset clip-space y (PS1 vertical pan effect)
    # clip.y += y_shear * clip.w  →  after perspective divide: NDC.y += y_shear
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_YSHEAR,      ID_PSX_OPTS, 1)
    s.emit(OP_F_MUL,             ID_FLOAT, ID_YSHEAR_TERM, ID_YSHEAR, ID_RPOS_W)
    s.emit(OP_F_ADD,             ID_FLOAT, ID_SY_SHEARED,  ID_SY, ID_YSHEAR_TERM)
    # Build final position and write gl_Position
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_RPOS_FINAL,
           ID_SX, ID_SY_SHEARED, ID_RPOS_Z, ID_RPOS_W)
    s.emit(OP_ACCESS_CHAIN, ID_PTR_OUT_V4, ID_P_GPOS, ID_GL_POS_BLOCK, ID_CI0)
    s.emit(OP_STORE, ID_P_GPOS, ID_RPOS_FINAL)

    # Pass-through normal
    s.emit(OP_LOAD,  ID_VEC3, ID_LNRM, ID_IN_NRM)
    s.emit(OP_STORE, ID_OUT_NRM, ID_LNRM)

    # Pass-through UV
    s.emit(OP_LOAD,  ID_VEC2, ID_LUV, ID_IN_UV)
    s.emit(OP_STORE, ID_OUT_UV, ID_LUV)

    # Pass-through texIndex
    s.emit(OP_LOAD,  ID_FLOAT, ID_LTIDX, ID_IN_TEXIDX)
    s.emit(OP_STORE, ID_OUT_TEXIDX, ID_LTIDX)

    # Pass-through light color (vec3)
    s.emit(OP_LOAD,  ID_VEC3, ID_LLIGHT, ID_IN_LIGHT)
    s.emit(OP_STORE, ID_OUT_LIGHT, ID_LLIGHT)

    # Pass-through AO
    s.emit(OP_LOAD,  ID_FLOAT, ID_LAO, ID_IN_AO)
    s.emit(OP_STORE, ID_OUT_AO, ID_LAO)

    # Affine UV / texIdx outputs (same values; NoPerspective decoration means the
    # GPU interpolates them linearly without perspective divide in the fragment stage)
    s.emit(OP_STORE, ID_OUT_AFF_UV,     ID_LUV)    # ID_LUV loaded in pass-through above
    s.emit(OP_STORE, ID_OUT_AFF_TEXIDX, ID_LTIDX)  # ID_LTIDX loaded in pass-through above

    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)

    return s.build()


# ── Fragment shader ────────────────────────────────────────────────────────────
def build_fragment():
    """
    GLSL equivalent:
        layout(set=2, binding=0) uniform sampler2DArray tex;
        layout(location=0) in vec3  fragNormal;
        layout(location=1) in vec2  fragUV;
        layout(location=2) in float fragTexIndex;
        layout(location=3) in vec3  fragLightColor; // pre-multiplied RGB light (0..1)
        layout(location=4) in float fragAO;         // per-vertex ambient occlusion
        layout(location=0) out vec4 outColor;

        layout(set=3, binding=0) uniform LightUBO {
            vec4 opts;  // x=fullbright(0/1), y=ao_mix, z=ambient_floor, w=unused
        } ubo;

        void main() {
            vec3  L    = vec3(0.57735, 0.57735, 0.57735);
            float diff = dot(fragNormal, L) * 0.45 + 0.55;

            float fullbright = ubo.opts.x;
            float ao_mix     = ubo.opts.y;
            float ambient    = ubo.opts.z;

            // Light color: clamp each channel to at least ambient level
            vec3 ambV = vec3(ambient);
            vec3 effColor = fullbright > 0.5 ? vec3(1.0) : max(fragLightColor, ambV);

            float aoFactor = (1.0 - ao_mix) + fragAO * ao_mix;

            vec4 tc = texture(tex, vec3(fragUV, fragTexIndex));
            if (tc.a < 0.05) discard;
            outColor = vec4(tc.rgb * effColor * diff * aoFactor, tc.a);
        }
    """
    s = Spirv()

    # ── IDs ───────────────────────────────────────────────────────────────────
    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_INT          = s.new()
    ID_VEC2         = s.new()
    ID_VEC3         = s.new()
    ID_VEC4         = s.new()
    ID_IMAGE        = s.new()
    ID_SAMP_IMG     = s.new()
    ID_PTR_UC_SIMG  = s.new()
    ID_PTR_IN_V3    = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_IN_F     = s.new()
    ID_PTR_OUT_V4   = s.new()
    # Fragment UBO: { vec4 opts } at set=3, binding=0
    ID_LIGHT_STRUCT = s.new()
    ID_PTR_UNI_UBO  = s.new()
    ID_PTR_UNI_V4   = s.new()
    ID_SAMPLER_VAR  = s.new()
    ID_LIGHT_VAR    = s.new()
    ID_FRAG_NRM     = s.new()   # location=0
    ID_FRAG_UV      = s.new()   # location=1
    ID_FRAG_TEXIDX  = s.new()   # location=2
    ID_FRAG_LIGHT   = s.new()   # location=3  vec3 light color
    ID_FRAG_AO      = s.new()   # location=4  float AO
    ID_OUT_COL      = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    ID_CI0          = s.new()
    # Float / vec constants
    ID_LC           = s.new()   # 0.57735
    ID_LIGHT_VEC    = s.new()   # vec3(lc,lc,lc) for diffuse
    ID_C0           = s.new()   # 0.0
    ID_C045         = s.new()
    ID_C055         = s.new()
    ID_C05          = s.new()   # 0.5
    ID_C1           = s.new()   # 1.0
    ID_ALPHA_THRESH = s.new()   # 0.05
    ID_BOOL         = s.new()
    # Runtime – diffuse
    ID_LNRM         = s.new()
    ID_DIFF_DOT     = s.new()
    ID_DIFF_SCALED  = s.new()
    ID_BIASED       = s.new()
    # Per-axis diffuse shading (Minecraft-style)
    ID_NRM_X        = s.new()
    ID_NRM_Y        = s.new()
    ID_NRM_Z        = s.new()
    ID_Y_SQ         = s.new()
    ID_X_CONTRIB    = s.new()
    ID_Z_CONTRIB    = s.new()
    ID_Y_GT_ZERO    = s.new()
    ID_Y_FACTOR     = s.new()
    ID_Y_CONTRIB    = s.new()
    ID_BIASED_TMP   = s.new()
    # Runtime – UBO
    ID_P_OPTS       = s.new()
    ID_OPTS_VEC4    = s.new()
    ID_FULLBRIGHT   = s.new()
    ID_AO_MIX       = s.new()
    ID_AMBIENT      = s.new()
    # Runtime – light (vec3)
    ID_LIGHT_COL    = s.new()   # loaded fragLightColor vec3
    ID_AMB_R        = s.new()   # ambient as float component
    ID_AMB_VEC      = s.new()   # vec3(ambient, ambient, ambient)
    ID_IS_FULL      = s.new()
    ID_ONE_VEC      = s.new()   # const vec3(1,1,1)
    ID_EFF_COL_NOFL = s.new()   # max(fragLightColor, ambV)
    ID_EFF_COL      = s.new()   # select(fullbright>0.5, vec3(1), above)
    # Runtime – diffuse × effColor
    ID_LC_R         = s.new()
    ID_LC_G         = s.new()
    ID_LC_B         = s.new()
    ID_LIT_R        = s.new()
    ID_LIT_G        = s.new()
    ID_LIT_B        = s.new()
    # Runtime – AO
    ID_LAO          = s.new()
    ID_INV_AO_MIX   = s.new()
    ID_SCALED_AO    = s.new()
    ID_AO_FACTOR    = s.new()
    # Runtime – texture
    ID_LUV          = s.new()
    ID_LTIDX        = s.new()
    ID_UVX          = s.new()
    ID_UVY          = s.new()
    ID_UVW          = s.new()
    ID_SIMG_LOADED  = s.new()
    ID_TC           = s.new()
    ID_TC_R         = s.new()
    ID_TC_G         = s.new()
    ID_TC_B         = s.new()
    ID_TC_A         = s.new()
    # Runtime – output  (tc * effColor * diff * aoFactor)
    ID_TX_R         = s.new()
    ID_TX_G         = s.new()
    ID_TX_B         = s.new()
    ID_TD_R         = s.new()
    ID_TD_G         = s.new()
    ID_TD_B         = s.new()
    ID_TMP_R        = s.new()
    ID_TMP_G        = s.new()
    ID_TMP_B        = s.new()
    ID_OUT_R        = s.new()
    ID_OUT_G        = s.new()
    ID_OUT_B        = s.new()
    ID_OUT_COLOR    = s.new()
    # Alpha test labels
    ID_ALPHA_CMP    = s.new()
    ID_DISCARD_LBL  = s.new()
    ID_CONTINUE_LBL = s.new()
    # Max per-component (ExtInst)
    ID_GLSL_EXT     = s.new()
    ID_EFF_R        = s.new()   # max(lightR, ambient)
    ID_EFF_G        = s.new()
    ID_EFF_B        = s.new()
    ID_AMB_R2        = s.new()   # ambient.g component
    ID_AMB_G2        = s.new()   # ambient.b component
    # Affine texture mapping
    ID_FRAG_AFF_UV   = s.new()   # location 5, NoPerspective vec2
    ID_FRAG_AFF_TIDX = s.new()   # location 6, NoPerspective float
    ID_AFFINE_MIX    = s.new()   # opts.w  (0 = perspective-correct, 1 = fully affine)
    ID_LAFF_UV       = s.new()   # loaded affine UV
    ID_LAFF_TIDX     = s.new()   # loaded affine texIdx
    ID_LAFF_UX       = s.new()   # affine UV x component
    ID_LAFF_UY       = s.new()   # affine UV y component
    ID_PERSP_UX      = s.new()   # perspective UV x
    ID_PERSP_UY      = s.new()   # perspective UV y
    ID_MIX_UX        = s.new()   # FMix result x
    ID_MIX_UY        = s.new()   # FMix result y
    ID_MIX_TIDX      = s.new()   # FMix result texIdx

    # ── Capabilities ─────────────────────────────────────────────────────────
    s.emit(OP_CAPABILITY, CAP_SHADER)

    # ── Extension import (GLSL.std.450 for FMax + FMix) ──────────────────────────────
    OP_EXT_INST_IMPORT = 11
    ext_words = str_words("GLSL.std.450")
    wc = 1 + 1 + len(ext_words)
    s.words.append(wc << 16 | OP_EXT_INST_IMPORT)
    s.words.append(ID_GLSL_EXT)
    s.words.extend(ext_words)

    # ── Memory model ──────────────────────────────────────────────────────────
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)

    # ── Entry point ───────────────────────────────────────────────────────────
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main",
                  ID_FRAG_NRM, ID_FRAG_UV, ID_FRAG_TEXIDX, ID_FRAG_LIGHT, ID_FRAG_AO,
                  ID_FRAG_AFF_UV, ID_FRAG_AFF_TIDX, ID_OUT_COL)

    # ── Execution mode ────────────────────────────────────────────────────────
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    # ── Decorations ───────────────────────────────────────────────────────────
    s.emit(OP_DECORATE, ID_FRAG_NRM,    DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_FRAG_UV,     DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_FRAG_TEXIDX, DEC_LOCATION, 2)
    s.emit(OP_DECORATE, ID_FRAG_LIGHT,  DEC_LOCATION, 3)
    s.emit(OP_DECORATE, ID_FRAG_AO,       DEC_LOCATION, 4)
    s.emit(OP_DECORATE, ID_FRAG_AFF_UV,   DEC_LOCATION, 5)
    s.emit(OP_DECORATE, ID_FRAG_AFF_UV,   DEC_NO_PERSPECTIVE)
    s.emit(OP_DECORATE, ID_FRAG_AFF_TIDX, DEC_LOCATION, 6)
    s.emit(OP_DECORATE, ID_FRAG_AFF_TIDX, DEC_NO_PERSPECTIVE)
    s.emit(OP_DECORATE, ID_OUT_COL,       DEC_LOCATION, 0)
    # Texture sampler (set=2, binding=0)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_BINDING,        0)
    # LightUBO (set=3, binding=0) — single vec4 opts
    s.emit(OP_DECORATE,        ID_LIGHT_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_LIGHT_STRUCT, 0, DEC_OFFSET, 0)
    s.emit(OP_DECORATE, ID_LIGHT_VAR, DEC_DESCRIPTOR_SET, 3)
    s.emit(OP_DECORATE, ID_LIGHT_VAR, DEC_BINDING,        0)

    # ── Types + constants + global variables ──────────────────────────────────
    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_INT,      ID_INT, 32, 1)
    s.emit(OP_TYPE_VECTOR,   ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,   ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_BOOL,     ID_BOOL)
    s.emit(OP_TYPE_IMAGE,         ID_IMAGE,    ID_FLOAT, 1, 0, 1, 0, 1, 0)
    s.emit(OP_TYPE_SAMPLED_IMAGE, ID_SAMP_IMG, ID_IMAGE)
    s.emit(OP_TYPE_POINTER, ID_PTR_UC_SIMG, SC_UNIFORM_CONSTANT, ID_SAMP_IMG)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V3,   SC_INPUT,  ID_VEC3)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,   SC_INPUT,  ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_F,    SC_INPUT,  ID_FLOAT)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4,  SC_OUTPUT, ID_VEC4)
    # LightUBO: single vec4
    s.emit(OP_TYPE_STRUCT,  ID_LIGHT_STRUCT, ID_VEC4)
    s.emit(OP_TYPE_POINTER, ID_PTR_UNI_UBO,  SC_UNIFORM, ID_LIGHT_STRUCT)
    s.emit(OP_TYPE_POINTER, ID_PTR_UNI_V4,   SC_UNIFORM, ID_VEC4)

    # Global variables
    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_SAMPLER_VAR, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_UNI_UBO, ID_LIGHT_VAR,   SC_UNIFORM)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,   ID_FRAG_NRM,    SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_FRAG_UV,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_F,    ID_FRAG_TEXIDX, SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,   ID_FRAG_LIGHT,   SC_INPUT)   # vec3 now
    s.emit(OP_VARIABLE, ID_PTR_IN_F,    ID_FRAG_AO,      SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_FRAG_AFF_UV,  SC_INPUT)   # NoPerspective affine UV
    s.emit(OP_VARIABLE, ID_PTR_IN_F,    ID_FRAG_AFF_TIDX,SC_INPUT)   # NoPerspective affine texIdx
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4,  ID_OUT_COL,      SC_OUTPUT)

    # Constants
    s.emit(OP_CONSTANT, ID_INT,   ID_CI0,  0)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_LC,   f2w(0.8))   # kX: shade for ±X-axis wall faces
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C0,   f2w(0.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C045, f2w(0.6))   # kZ: shade for ±Z-axis wall faces
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C055, f2w(0.55))  # (kept for SPIR-V ID validity)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C05,  f2w(0.5))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C1,   f2w(1.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_ALPHA_THRESH, f2w(0.05))
    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC3, ID_LIGHT_VEC, ID_LC, ID_LC, ID_LC)  # unused, kept for SPIR-V ID validity
    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC3, ID_ONE_VEC,   ID_C1, ID_C1, ID_C1)

    # ── Function ──────────────────────────────────────────────────────────────
    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)

    # ── Per-axis diffuse shading (Minecraft-style) ──────────────────────────────
    # ±X faces → 0.8, ±Z faces → 0.6, +Y → 1.0, -Y → 0.5
    # Uses n² trick: for axis-aligned normals n.x²=1 on X faces only, etc.
    s.emit(OP_LOAD,              ID_VEC3,  ID_LNRM,      ID_FRAG_NRM)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_NRM_X,     ID_LNRM, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_NRM_Y,     ID_LNRM, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_NRM_Z,     ID_LNRM, 2)
    s.emit(OP_F_MUL, ID_FLOAT, ID_DIFF_DOT,    ID_NRM_X, ID_NRM_X)   # x² = 1 on X faces
    s.emit(OP_F_MUL, ID_FLOAT, ID_DIFF_SCALED, ID_NRM_Z, ID_NRM_Z)   # z² = 1 on Z faces
    s.emit(OP_F_MUL, ID_FLOAT, ID_Y_SQ,        ID_NRM_Y, ID_NRM_Y)   # y² = 1 on Y faces
    s.emit(OP_F_MUL, ID_FLOAT, ID_X_CONTRIB,   ID_DIFF_DOT,    ID_LC)    # x² * 0.8
    s.emit(OP_F_MUL, ID_FLOAT, ID_Z_CONTRIB,   ID_DIFF_SCALED, ID_C045)  # z² * 0.6
    s.emit(OP_F_ORD_GREATER_THAN, ID_BOOL,  ID_Y_GT_ZERO, ID_NRM_Y, ID_C0)
    s.emit(OP_SELECT,             ID_FLOAT, ID_Y_FACTOR,  ID_Y_GT_ZERO, ID_C1, ID_C05)  # 1.0 top, 0.5 bottom
    s.emit(OP_F_MUL, ID_FLOAT, ID_Y_CONTRIB,  ID_Y_SQ, ID_Y_FACTOR)
    s.emit(OP_F_ADD, ID_FLOAT, ID_BIASED_TMP, ID_X_CONTRIB, ID_Y_CONTRIB)
    s.emit(OP_F_ADD, ID_FLOAT, ID_BIASED,     ID_BIASED_TMP, ID_Z_CONTRIB)

    # ── Load UBO opts ─────────────────────────────────────────────────────────
    s.emit(OP_ACCESS_CHAIN, ID_PTR_UNI_V4, ID_P_OPTS, ID_LIGHT_VAR, ID_CI0)
    s.emit(OP_LOAD, ID_VEC4, ID_OPTS_VEC4, ID_P_OPTS)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_FULLBRIGHT, ID_OPTS_VEC4, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_AO_MIX,     ID_OPTS_VEC4, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_AMBIENT,    ID_OPTS_VEC4, 2)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_AFFINE_MIX, ID_OPTS_VEC4, 3)

    # ── Load per-vertex light color (vec3) + AO ───────────────────────────────
    s.emit(OP_LOAD, ID_VEC3,  ID_LIGHT_COL, ID_FRAG_LIGHT)
    s.emit(OP_LOAD, ID_FLOAT, ID_LAO,       ID_FRAG_AO)

    # ── Build ambient vec3(ambient, ambient, ambient) ─────────────────────────
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC3, ID_AMB_VEC, ID_AMBIENT, ID_AMBIENT, ID_AMBIENT)

    # ── effColor = fullbright > 0.5 ? vec3(1) : max(fragLightColor, ambV) ─────
    # Use component-wise FMax via OpExtInst GLSL FMax (op 40)
    GLSL_FMAX = 40
    # Extract light components
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_LC_R, ID_LIGHT_COL, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_LC_G, ID_LIGHT_COL, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_LC_B, ID_LIGHT_COL, 2)
    # Extract ambient components
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_AMB_R,  ID_AMB_VEC, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_AMB_R2, ID_AMB_VEC, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_AMB_G2, ID_AMB_VEC, 2)
    s.emit(OP_EXT_INST, ID_FLOAT, ID_EFF_R, ID_GLSL_EXT, GLSL_FMAX, ID_LC_R, ID_AMB_R)
    s.emit(OP_EXT_INST, ID_FLOAT, ID_EFF_G, ID_GLSL_EXT, GLSL_FMAX, ID_LC_G, ID_AMB_R2)
    s.emit(OP_EXT_INST, ID_FLOAT, ID_EFF_B, ID_GLSL_EXT, GLSL_FMAX, ID_LC_B, ID_AMB_G2)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC3, ID_EFF_COL_NOFL, ID_EFF_R, ID_EFF_G, ID_EFF_B)
    # Fullbright select: effColor = fullbright > 0.5 ? vec3(1.0) : EFF_COL_NOFL
    s.emit(OP_F_ORD_GREATER_THAN, ID_BOOL, ID_IS_FULL, ID_FULLBRIGHT, ID_C05)
    s.emit(OP_SELECT, ID_VEC3, ID_EFF_COL, ID_IS_FULL, ID_ONE_VEC, ID_EFF_COL_NOFL)

    # ── AO factor ─────────────────────────────────────────────────────────────
    s.emit(OP_F_SUB, ID_FLOAT, ID_INV_AO_MIX, ID_C1, ID_AO_MIX)
    s.emit(OP_F_MUL, ID_FLOAT, ID_SCALED_AO,  ID_LAO, ID_AO_MIX)
    s.emit(OP_F_ADD, ID_FLOAT, ID_AO_FACTOR,  ID_INV_AO_MIX, ID_SCALED_AO)

    # ── Texture sample (affine + perspective blend) ─────────────────────────
    s.emit(OP_LOAD,              ID_VEC2,  ID_LUV,       ID_FRAG_UV)
    s.emit(OP_LOAD,              ID_FLOAT, ID_LTIDX,     ID_FRAG_TEXIDX)
    s.emit(OP_LOAD,              ID_VEC2,  ID_LAFF_UV,   ID_FRAG_AFF_UV)
    s.emit(OP_LOAD,              ID_FLOAT, ID_LAFF_TIDX, ID_FRAG_AFF_TIDX)
    # Extract perspective UV components
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_PERSP_UX, ID_LUV, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_PERSP_UY, ID_LUV, 1)
    # Extract affine UV components
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_LAFF_UX, ID_LAFF_UV, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_LAFF_UY, ID_LAFF_UV, 1)
    # FMix: mix(persp, affine, affine_mix) — 0=perspective-correct, 1=pure affine warp
    GLSL_FMIX = 46
    s.emit(OP_EXT_INST, ID_FLOAT, ID_MIX_UX,   ID_GLSL_EXT, GLSL_FMIX, ID_PERSP_UX,  ID_LAFF_UX,   ID_AFFINE_MIX)
    s.emit(OP_EXT_INST, ID_FLOAT, ID_MIX_UY,   ID_GLSL_EXT, GLSL_FMIX, ID_PERSP_UY,  ID_LAFF_UY,   ID_AFFINE_MIX)
    s.emit(OP_EXT_INST, ID_FLOAT, ID_MIX_TIDX, ID_GLSL_EXT, GLSL_FMIX, ID_LTIDX,     ID_LAFF_TIDX, ID_AFFINE_MIX)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC3, ID_UVW, ID_MIX_UX, ID_MIX_UY, ID_MIX_TIDX)
    s.emit(OP_LOAD, ID_SAMP_IMG, ID_SIMG_LOADED, ID_SAMPLER_VAR)
    s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, ID_TC, ID_SIMG_LOADED, ID_UVW)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_R, ID_TC, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_G, ID_TC, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_B, ID_TC, 2)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_A, ID_TC, 3)

    # Alpha test
    s.emit(OP_F_ORD_LESS_THAN,    ID_BOOL,  ID_ALPHA_CMP,   ID_TC_A, ID_ALPHA_THRESH)
    s.emit(OP_SELECTION_MERGE,    ID_CONTINUE_LBL, 0)
    s.emit(OP_BRANCH_CONDITIONAL, ID_ALPHA_CMP, ID_DISCARD_LBL, ID_CONTINUE_LBL)
    s.emit(OP_LABEL,              ID_DISCARD_LBL)
    s.emit(OP_KILL)
    s.emit(OP_LABEL,              ID_CONTINUE_LBL)

    # ── Apply: tc.rgb * effColor * diff * aoFactor ────────────────────────────
    # Extract effColor components
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_LIT_R, ID_EFF_COL, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_LIT_G, ID_EFF_COL, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_LIT_B, ID_EFF_COL, 2)
    # tc * effColor
    s.emit(OP_F_MUL, ID_FLOAT, ID_TX_R, ID_TC_R, ID_LIT_R)
    s.emit(OP_F_MUL, ID_FLOAT, ID_TX_G, ID_TC_G, ID_LIT_G)
    s.emit(OP_F_MUL, ID_FLOAT, ID_TX_B, ID_TC_B, ID_LIT_B)
    # * diff
    s.emit(OP_F_MUL, ID_FLOAT, ID_TD_R, ID_TX_R, ID_BIASED)
    s.emit(OP_F_MUL, ID_FLOAT, ID_TD_G, ID_TX_G, ID_BIASED)
    s.emit(OP_F_MUL, ID_FLOAT, ID_TD_B, ID_TX_B, ID_BIASED)
    # * aoFactor
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_R, ID_TD_R, ID_AO_FACTOR)
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_G, ID_TD_G, ID_AO_FACTOR)
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_B, ID_TD_B, ID_AO_FACTOR)

    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_OUT_COLOR,
           ID_OUT_R, ID_OUT_G, ID_OUT_B, ID_TC_A)
    s.emit(OP_STORE, ID_OUT_COL, ID_OUT_COLOR)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)

    return s.build()


    s = Spirv()

    # ── IDs ───────────────────────────────────────────────────────────────────
    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_INT          = s.new()
    ID_VEC2         = s.new()
    ID_VEC3         = s.new()
    ID_VEC4         = s.new()
    ID_IMAGE        = s.new()
    ID_SAMP_IMG     = s.new()
    ID_PTR_UC_SIMG  = s.new()
    ID_PTR_IN_V3    = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_IN_F     = s.new()
    ID_PTR_OUT_V4   = s.new()
    # Fragment UBO: { vec4 opts } at set=3, binding=0
    ID_LIGHT_STRUCT = s.new()   # TypeStruct(vec4)
    ID_PTR_UNI_UBO  = s.new()
    ID_PTR_UNI_V4   = s.new()
    ID_SAMPLER_VAR  = s.new()
    ID_LIGHT_VAR    = s.new()
    ID_FRAG_NRM     = s.new()   # location=0
    ID_FRAG_UV      = s.new()   # location=1
    ID_FRAG_TEXIDX  = s.new()   # location=2
    ID_FRAG_LIGHT   = s.new()   # location=3  dynamic light
    ID_FRAG_AO      = s.new()   # location=4  ambient occlusion
    ID_OUT_COL      = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    ID_CI0          = s.new()   # int 0
    # Float constants
    ID_LC           = s.new()   # 0.57735
    ID_LIGHT_VEC    = s.new()   # vec3(lc,lc,lc)
    ID_C0           = s.new()   # 0.0
    ID_C045         = s.new()
    ID_C055         = s.new()
    ID_C05          = s.new()   # 0.5  (fullbright threshold)
    ID_C1           = s.new()   # 1.0
    ID_ALPHA_THRESH = s.new()   # 0.05
    # Runtime – diffuse
    ID_LNRM         = s.new()
    ID_DIFF_DOT     = s.new()
    ID_DIFF_SCALED  = s.new()
    ID_BIASED       = s.new()
    # Runtime – UBO
    ID_P_OPTS       = s.new()
    ID_OPTS_VEC4    = s.new()
    ID_FULLBRIGHT   = s.new()
    ID_AO_MIX       = s.new()
    ID_AMBIENT      = s.new()
    # Runtime – light and AO
    ID_L_LIGHT      = s.new()
    ID_LAO          = s.new()
    ID_BOOL         = s.new()
    ID_LIGHT_GT_AMB = s.new()
    ID_MAX_LIGHT    = s.new()
    ID_IS_FULL      = s.new()
    ID_EFF_LIGHT    = s.new()
    # Runtime – combine
    ID_COMBINED     = s.new()
    ID_GT_ZERO      = s.new()
    ID_CLAMP_LO     = s.new()
    ID_LT_ONE       = s.new()
    ID_FINAL_LIGHT  = s.new()
    # Runtime – AO
    ID_INV_AO_MIX   = s.new()
    ID_SCALED_AO    = s.new()
    ID_AO_FACTOR    = s.new()
    # Runtime – texture
    ID_LUV          = s.new()
    ID_LTIDX        = s.new()
    ID_UVX          = s.new()
    ID_UVY          = s.new()
    ID_UVW          = s.new()
    ID_SIMG_LOADED  = s.new()
    ID_TC           = s.new()
    ID_TC_R         = s.new()
    ID_TC_G         = s.new()
    ID_TC_B         = s.new()
    ID_TC_A         = s.new()
    # Runtime – output
    ID_TMP_R        = s.new()
    ID_TMP_G        = s.new()
    ID_TMP_B        = s.new()
    ID_OUT_R        = s.new()
    ID_OUT_G        = s.new()
    ID_OUT_B        = s.new()
    ID_OUT_COLOR    = s.new()
    # Alpha test labels
    ID_ALPHA_CMP    = s.new()
    ID_DISCARD_LBL  = s.new()
    ID_CONTINUE_LBL = s.new()

    # ── Capabilities ─────────────────────────────────────────────────────────
    s.emit(OP_CAPABILITY, CAP_SHADER)

    # ── Memory model ──────────────────────────────────────────────────────────
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)

    # ── Entry point ───────────────────────────────────────────────────────────
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main",
                  ID_FRAG_NRM, ID_FRAG_UV, ID_FRAG_TEXIDX, ID_FRAG_LIGHT, ID_FRAG_AO,
                  ID_OUT_COL)

    # ── Execution mode ────────────────────────────────────────────────────────
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    # ── Decorations ───────────────────────────────────────────────────────────
    s.emit(OP_DECORATE, ID_FRAG_NRM,    DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_FRAG_UV,     DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_FRAG_TEXIDX, DEC_LOCATION, 2)
    s.emit(OP_DECORATE, ID_FRAG_LIGHT,  DEC_LOCATION, 3)
    s.emit(OP_DECORATE, ID_FRAG_AO,     DEC_LOCATION, 4)
    s.emit(OP_DECORATE, ID_OUT_COL,     DEC_LOCATION, 0)
    # Texture sampler (set=2, binding=0)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_BINDING,        0)
    # LightUBO (set=3, binding=0) — single vec4 opts
    s.emit(OP_DECORATE,        ID_LIGHT_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_LIGHT_STRUCT, 0, DEC_OFFSET, 0)
    s.emit(OP_DECORATE, ID_LIGHT_VAR, DEC_DESCRIPTOR_SET, 3)
    s.emit(OP_DECORATE, ID_LIGHT_VAR, DEC_BINDING,        0)

    # ── Types + constants + global variables ──────────────────────────────────
    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_INT,      ID_INT, 32, 1)
    s.emit(OP_TYPE_VECTOR,   ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,   ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_BOOL,     ID_BOOL)
    s.emit(OP_TYPE_IMAGE,         ID_IMAGE,    ID_FLOAT, 1, 0, 1, 0, 1, 0)
    s.emit(OP_TYPE_SAMPLED_IMAGE, ID_SAMP_IMG, ID_IMAGE)
    s.emit(OP_TYPE_POINTER, ID_PTR_UC_SIMG, SC_UNIFORM_CONSTANT, ID_SAMP_IMG)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V3,   SC_INPUT,  ID_VEC3)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,   SC_INPUT,  ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_F,    SC_INPUT,  ID_FLOAT)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4,  SC_OUTPUT, ID_VEC4)
    # LightUBO: single vec4
    s.emit(OP_TYPE_STRUCT,  ID_LIGHT_STRUCT, ID_VEC4)
    s.emit(OP_TYPE_POINTER, ID_PTR_UNI_UBO,  SC_UNIFORM, ID_LIGHT_STRUCT)
    s.emit(OP_TYPE_POINTER, ID_PTR_UNI_V4,   SC_UNIFORM, ID_VEC4)

    # Global variables
    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_SAMPLER_VAR, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_UNI_UBO, ID_LIGHT_VAR,   SC_UNIFORM)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,   ID_FRAG_NRM,    SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_FRAG_UV,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_F,    ID_FRAG_TEXIDX, SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_F,    ID_FRAG_LIGHT,  SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_F,    ID_FRAG_AO,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4,  ID_OUT_COL,     SC_OUTPUT)

    # Constants
    s.emit(OP_CONSTANT, ID_INT,   ID_CI0,  0)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_LC,   f2w(0.57735))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C0,   f2w(0.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C045, f2w(0.45))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C055, f2w(0.55))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C05,  f2w(0.5))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C1,   f2w(1.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_ALPHA_THRESH, f2w(0.05))
    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC3, ID_LIGHT_VEC, ID_LC, ID_LC, ID_LC)

    # ── Function ──────────────────────────────────────────────────────────────
    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)

    # ── Lambertian diffuse ────────────────────────────────────────────────────
    s.emit(OP_LOAD,  ID_VEC3,  ID_LNRM,        ID_FRAG_NRM)
    s.emit(OP_DOT,   ID_FLOAT, ID_DIFF_DOT,    ID_LNRM, ID_LIGHT_VEC)
    s.emit(OP_F_MUL, ID_FLOAT, ID_DIFF_SCALED, ID_DIFF_DOT, ID_C045)
    s.emit(OP_F_ADD, ID_FLOAT, ID_BIASED,       ID_DIFF_SCALED, ID_C055)

    # ── Load UBO opts ─────────────────────────────────────────────────────────
    s.emit(OP_ACCESS_CHAIN, ID_PTR_UNI_V4, ID_P_OPTS, ID_LIGHT_VAR, ID_CI0)
    s.emit(OP_LOAD, ID_VEC4, ID_OPTS_VEC4, ID_P_OPTS)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_FULLBRIGHT, ID_OPTS_VEC4, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_AO_MIX,    ID_OPTS_VEC4, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_AMBIENT,   ID_OPTS_VEC4, 2)

    # ── Load per-vertex inputs ─────────────────────────────────────────────────
    s.emit(OP_LOAD, ID_FLOAT, ID_L_LIGHT, ID_FRAG_LIGHT)
    s.emit(OP_LOAD, ID_FLOAT, ID_LAO,     ID_FRAG_AO)

    # ── Dynamic light: max(fragLight, ambient) ─────────────────────────────────
    s.emit(OP_F_ORD_GREATER_THAN, ID_BOOL,  ID_LIGHT_GT_AMB, ID_L_LIGHT, ID_AMBIENT)
    s.emit(OP_SELECT,             ID_FLOAT, ID_MAX_LIGHT, ID_LIGHT_GT_AMB, ID_L_LIGHT, ID_AMBIENT)

    # ── Fullbright override: effLight = fullbright > 0.5 ? 1.0 : max_light ────
    s.emit(OP_F_ORD_GREATER_THAN, ID_BOOL,  ID_IS_FULL,   ID_FULLBRIGHT, ID_C05)
    s.emit(OP_SELECT,             ID_FLOAT, ID_EFF_LIGHT, ID_IS_FULL, ID_C1, ID_MAX_LIGHT)

    # ── finalLight = clamp(diff * effLight, 0, 1) ─────────────────────────────
    s.emit(OP_F_MUL,           ID_FLOAT, ID_COMBINED,    ID_BIASED, ID_EFF_LIGHT)
    s.emit(OP_F_ORD_GREATER_THAN, ID_BOOL,  ID_GT_ZERO,  ID_COMBINED, ID_C0)
    s.emit(OP_SELECT,          ID_FLOAT, ID_CLAMP_LO,    ID_GT_ZERO, ID_COMBINED, ID_C0)
    s.emit(OP_F_ORD_LESS_THAN, ID_BOOL,  ID_LT_ONE,      ID_CLAMP_LO, ID_C1)
    s.emit(OP_SELECT,          ID_FLOAT, ID_FINAL_LIGHT,  ID_LT_ONE, ID_CLAMP_LO, ID_C1)

    # ── AO factor: (1 - ao_mix) + fragAO * ao_mix ────────────────────────────
    s.emit(OP_F_SUB, ID_FLOAT, ID_INV_AO_MIX, ID_C1, ID_AO_MIX)
    s.emit(OP_F_MUL, ID_FLOAT, ID_SCALED_AO,  ID_LAO, ID_AO_MIX)
    s.emit(OP_F_ADD, ID_FLOAT, ID_AO_FACTOR,  ID_INV_AO_MIX, ID_SCALED_AO)

    # ── Texture sample ────────────────────────────────────────────────────────
    s.emit(OP_LOAD,              ID_VEC2,  ID_LUV,   ID_FRAG_UV)
    s.emit(OP_LOAD,              ID_FLOAT, ID_LTIDX, ID_FRAG_TEXIDX)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_UVX,   ID_LUV, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_UVY,   ID_LUV, 1)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC3, ID_UVW,  ID_UVX, ID_UVY, ID_LTIDX)
    s.emit(OP_LOAD, ID_SAMP_IMG, ID_SIMG_LOADED, ID_SAMPLER_VAR)
    s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, ID_TC, ID_SIMG_LOADED, ID_UVW)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_R, ID_TC, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_G, ID_TC, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_B, ID_TC, 2)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_A, ID_TC, 3)

    # Alpha test
    s.emit(OP_F_ORD_LESS_THAN,    ID_BOOL,  ID_ALPHA_CMP,   ID_TC_A, ID_ALPHA_THRESH)
    s.emit(OP_SELECTION_MERGE,    ID_CONTINUE_LBL, 0)
    s.emit(OP_BRANCH_CONDITIONAL, ID_ALPHA_CMP, ID_DISCARD_LBL, ID_CONTINUE_LBL)
    s.emit(OP_LABEL,              ID_DISCARD_LBL)
    s.emit(OP_KILL)
    s.emit(OP_LABEL,              ID_CONTINUE_LBL)

    # ── Apply lighting + AO ───────────────────────────────────────────────────
    s.emit(OP_F_MUL, ID_FLOAT, ID_TMP_R, ID_TC_R, ID_FINAL_LIGHT)
    s.emit(OP_F_MUL, ID_FLOAT, ID_TMP_G, ID_TC_G, ID_FINAL_LIGHT)
    s.emit(OP_F_MUL, ID_FLOAT, ID_TMP_B, ID_TC_B, ID_FINAL_LIGHT)
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_R, ID_TMP_R, ID_AO_FACTOR)
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_G, ID_TMP_G, ID_AO_FACTOR)
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_B, ID_TMP_B, ID_AO_FACTOR)

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
    Subtle white tint for face selection highlight:
        layout(location=0) out vec4 outColor;
        void main() { outColor = vec4(1.0, 1.0, 1.0, 0.12); }
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
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CG, f2w(1.00))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CB, f2w(1.00))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CA, f2w(0.12))
    # OP_CONSTANT_COMPOSITE must live in the global section, before any function.
    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC4, ID_COLOR, ID_CR, ID_CG, ID_CB, ID_CA)

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)
    s.emit(OP_STORE, ID_OUT_COL, ID_COLOR)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


# ── UI vertex shader ──────────────────────────────────────────────────────────
def build_ui_vertex():
    """
    GLSL equivalent:
        layout(location=0) in vec2 inPos;
        layout(location=1) in vec2 inUV;
        layout(location=2) in vec4 inColor;
        layout(location=0) out vec2 fragUV;
        layout(location=1) out vec4 fragColor;
        layout(set=1, binding=0) uniform UBO { vec4 xform; } ubo;
        // xform = {2/fb_w, -2/fb_h, -1.0, 1.0}
        out gl_PerVertex { vec4 gl_Position; };
        void main() {
            gl_Position = vec4(inPos.x * ubo.xform.x + ubo.xform.z,
                               inPos.y * ubo.xform.y + ubo.xform.w,
                               0.0, 1.0);
            fragUV    = inUV;
            fragColor = inColor;
        }
    """
    s = Spirv()

    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC4         = s.new()
    ID_INT          = s.new()
    ID_PV_STRUCT    = s.new()   # gl_PerVertex { vec4 gl_Position; }
    ID_UBO_STRUCT   = s.new()   # struct { vec4 xform; }
    ID_PTR_OUT_PV   = s.new()
    ID_PTR_UBO_S    = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_OUT_V2   = s.new()
    ID_PTR_IN_V4    = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_PTR_UNIF_V4  = s.new()   # pointer to vec4 in Uniform space
    ID_GL_POS_BLOCK = s.new()
    ID_UBO_VAR      = s.new()
    ID_IN_POS       = s.new()
    ID_IN_UV        = s.new()
    ID_IN_COLOR     = s.new()
    ID_OUT_UV       = s.new()
    ID_OUT_COLOR    = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    ID_CI0          = s.new()
    ID_CF0          = s.new()
    ID_CF1          = s.new()
    # Runtime
    ID_LPOS         = s.new()
    ID_P_XFORM      = s.new()
    ID_XFORM        = s.new()
    ID_X0           = s.new()
    ID_X1           = s.new()
    ID_X2           = s.new()
    ID_X3           = s.new()
    ID_P0           = s.new()
    ID_P1           = s.new()
    ID_T0           = s.new()
    ID_T1           = s.new()
    ID_NX           = s.new()
    ID_NY           = s.new()
    ID_POS4         = s.new()
    ID_P_GPOS       = s.new()
    ID_LUV          = s.new()
    ID_LCOL         = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_VERTEX, ID_MAIN, "main",
                  ID_IN_POS, ID_IN_UV, ID_IN_COLOR,
                  ID_GL_POS_BLOCK, ID_OUT_UV, ID_OUT_COLOR)

    s.emit(OP_DECORATE, ID_IN_POS,    DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_IN_UV,     DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_IN_COLOR,  DEC_LOCATION, 2)
    s.emit(OP_DECORATE, ID_OUT_UV,    DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_OUT_COLOR, DEC_LOCATION, 1)
    s.emit(OP_DECORATE,        ID_PV_STRUCT,  DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PV_STRUCT,  0, DEC_BUILT_IN, BI_POSITION)
    s.emit(OP_DECORATE,        ID_UBO_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_UBO_STRUCT, 0, DEC_OFFSET, 0)
    s.emit(OP_DECORATE,        ID_UBO_VAR,    DEC_DESCRIPTOR_SET, 1)
    s.emit(OP_DECORATE,        ID_UBO_VAR,    DEC_BINDING,        0)

    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,   ID_VEC2,  ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4,  ID_FLOAT, 4)
    s.emit(OP_TYPE_INT,      ID_INT,   32, 1)
    s.emit(OP_TYPE_STRUCT,   ID_PV_STRUCT,  ID_VEC4)
    s.emit(OP_TYPE_STRUCT,   ID_UBO_STRUCT, ID_VEC4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_PV,  SC_OUTPUT,  ID_PV_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_UBO_S,   SC_UNIFORM, ID_UBO_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V2,   SC_INPUT,   ID_VEC2)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V2,  SC_OUTPUT,  ID_VEC2)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V4,   SC_INPUT,   ID_VEC4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V4,  SC_OUTPUT,  ID_VEC4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_UNIF_V4, SC_UNIFORM, ID_VEC4)

    s.emit(OP_VARIABLE, ID_PTR_OUT_PV, ID_GL_POS_BLOCK, SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_UBO_S,  ID_UBO_VAR,      SC_UNIFORM)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,  ID_IN_POS,       SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,  ID_IN_UV,        SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V4,  ID_IN_COLOR,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V2, ID_OUT_UV,       SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4, ID_OUT_COLOR,    SC_OUTPUT)

    s.emit(OP_CONSTANT, ID_INT,   ID_CI0, 0)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CF0, f2w(0.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CF1, f2w(1.0))

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)

    # Load inPos, extract components
    s.emit(OP_LOAD,              ID_VEC2,  ID_LPOS, ID_IN_POS)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_P0,   ID_LPOS, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_P1,   ID_LPOS, 1)

    # Load xform from UBO member 0
    s.emit(OP_ACCESS_CHAIN,      ID_PTR_UNIF_V4, ID_P_XFORM, ID_UBO_VAR, ID_CI0)
    s.emit(OP_LOAD,              ID_VEC4,        ID_XFORM,   ID_P_XFORM)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_X0, ID_XFORM, 0)  # 2/w
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_X1, ID_XFORM, 1)  # -2/h
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_X2, ID_XFORM, 2)  # -1
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_X3, ID_XFORM, 3)  # +1

    # ndc = pos * scale + bias
    s.emit(OP_F_MUL, ID_FLOAT, ID_T0, ID_P0, ID_X0)
    s.emit(OP_F_ADD, ID_FLOAT, ID_NX, ID_T0, ID_X2)
    s.emit(OP_F_MUL, ID_FLOAT, ID_T1, ID_P1, ID_X1)
    s.emit(OP_F_ADD, ID_FLOAT, ID_NY, ID_T1, ID_X3)

    # gl_Position = vec4(nx, ny, 0, 1)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_POS4, ID_NX, ID_NY, ID_CF0, ID_CF1)
    s.emit(OP_ACCESS_CHAIN, ID_PTR_OUT_V4, ID_P_GPOS, ID_GL_POS_BLOCK, ID_CI0)
    s.emit(OP_STORE, ID_P_GPOS, ID_POS4)

    # fragUV = inUV
    s.emit(OP_LOAD,  ID_VEC2, ID_LUV,  ID_IN_UV)
    s.emit(OP_STORE, ID_OUT_UV, ID_LUV)

    # fragColor = inColor
    s.emit(OP_LOAD,  ID_VEC4, ID_LCOL, ID_IN_COLOR)
    s.emit(OP_STORE, ID_OUT_COLOR, ID_LCOL)

    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


# ── UI fragment shader ─────────────────────────────────────────────────────────
def build_ui_fragment():
    """
    GLSL equivalent:
        layout(set=2, binding=0) uniform sampler2D tex;
        layout(location=0) in vec2 fragUV;
        layout(location=1) in vec4 fragColor;
        layout(location=0) out vec4 outColor;
        void main() { outColor = texture(tex, fragUV) * fragColor; }
    """
    s = Spirv()

    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC4         = s.new()
    ID_IMAGE        = s.new()   # 2D non-arrayed sampled texture
    ID_SAMP_IMG     = s.new()
    ID_PTR_UC_SIMG  = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_IN_V4    = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_SAMPLER_VAR  = s.new()
    ID_FRAG_UV      = s.new()
    ID_FRAG_COLOR   = s.new()
    ID_OUT_COL      = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    # Runtime
    ID_LUV          = s.new()
    ID_LCOL         = s.new()
    ID_SIMG         = s.new()
    ID_TC           = s.new()
    ID_RESULT       = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main",
                  ID_FRAG_UV, ID_FRAG_COLOR, ID_OUT_COL)
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    s.emit(OP_DECORATE, ID_FRAG_UV,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_FRAG_COLOR,  DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_OUT_COL,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_BINDING,        0)

    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,   ID_VEC2,  ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4,  ID_FLOAT, 4)
    # 2D non-arrayed: Dim=1(2D), Depth=0, Arrayed=0, MS=0, Sampled=1, Format=0
    s.emit(OP_TYPE_IMAGE,         ID_IMAGE,    ID_FLOAT, 1, 0, 0, 0, 1, 0)
    s.emit(OP_TYPE_SAMPLED_IMAGE, ID_SAMP_IMG, ID_IMAGE)
    s.emit(OP_TYPE_POINTER, ID_PTR_UC_SIMG, SC_UNIFORM_CONSTANT, ID_SAMP_IMG)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,   SC_INPUT,             ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V4,   SC_INPUT,             ID_VEC4)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4,  SC_OUTPUT,            ID_VEC4)

    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_SAMPLER_VAR, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_FRAG_UV,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V4,   ID_FRAG_COLOR,  SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4,  ID_OUT_COL,     SC_OUTPUT)

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)

    s.emit(OP_LOAD, ID_VEC2,     ID_LUV,  ID_FRAG_UV)
    s.emit(OP_LOAD, ID_VEC4,     ID_LCOL, ID_FRAG_COLOR)
    s.emit(OP_LOAD, ID_SAMP_IMG, ID_SIMG, ID_SAMPLER_VAR)
    s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, ID_TC, ID_SIMG, ID_LUV)
    # outColor = texture_sample * vertexColor  (component-wise vec4 multiply)
    s.emit(OP_F_MUL, ID_VEC4, ID_RESULT, ID_TC, ID_LCOL)
    s.emit(OP_STORE, ID_OUT_COL, ID_RESULT)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


# ── MTSDF text fragment shader ───────────────────────────────────────────────
def build_mtsdf_fragment():
    """
    Bitmap text shader.  Samples the alpha channel of the glyph atlas and
    multiplies by the vertex colour so the caller controls text colour/opacity.

    GLSL equivalent:
        layout(set=2, binding=0) uniform sampler2D tex;
        layout(location=0) in  vec2 fragUV;
        layout(location=1) in  vec4 fragColor;
        layout(location=0) out vec4 outColor;

        void main() {
            float a  = texture(tex, fragUV).a;
            outColor = vec4(fragColor.rgb, fragColor.a * a);
        }
    """
    s = Spirv()

    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC4         = s.new()
    ID_IMAGE        = s.new()
    ID_SAMP_IMG     = s.new()
    ID_PTR_UC_SIMG  = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_IN_V4    = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_SAMPLER_VAR  = s.new()
    ID_FRAG_UV      = s.new()
    ID_FRAG_COLOR   = s.new()
    ID_OUT_COL      = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    # Runtime IDs
    ID_LUV          = s.new()
    ID_LCOL         = s.new()
    ID_SIMG         = s.new()
    ID_SAMPLE       = s.new()
    ID_SA           = s.new()   # texture .a
    ID_VCA          = s.new()   # fragColor.a
    ID_FINAL_A      = s.new()
    ID_VCR          = s.new()
    ID_VCG          = s.new()
    ID_VCB          = s.new()
    ID_RESULT       = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main",
                  ID_FRAG_UV, ID_FRAG_COLOR, ID_OUT_COL)
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    s.emit(OP_DECORATE, ID_FRAG_UV,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_FRAG_COLOR,  DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_OUT_COL,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_BINDING,        0)

    s.emit(OP_TYPE_VOID,          ID_VOID)
    s.emit(OP_TYPE_FUNCTION,      ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,         ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,        ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,        ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_IMAGE,         ID_IMAGE,    ID_FLOAT, 1, 0, 0, 0, 1, 0)
    s.emit(OP_TYPE_SAMPLED_IMAGE, ID_SAMP_IMG, ID_IMAGE)
    s.emit(OP_TYPE_POINTER, ID_PTR_UC_SIMG, SC_UNIFORM_CONSTANT, ID_SAMP_IMG)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,   SC_INPUT,             ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V4,   SC_INPUT,             ID_VEC4)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4,  SC_OUTPUT,            ID_VEC4)

    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_SAMPLER_VAR, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_FRAG_UV,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V4,   ID_FRAG_COLOR,  SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4,  ID_OUT_COL,     SC_OUTPUT)

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)

    # Load varyings
    s.emit(OP_LOAD, ID_VEC2,     ID_LUV,  ID_FRAG_UV)
    s.emit(OP_LOAD, ID_VEC4,     ID_LCOL, ID_FRAG_COLOR)
    s.emit(OP_LOAD, ID_SAMP_IMG, ID_SIMG, ID_SAMPLER_VAR)

    # Sample bitmap atlas
    s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, ID_SAMPLE, ID_SIMG, ID_LUV)

    # Extract alpha channel (component 3)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_SA, ID_SAMPLE, 3)

    # final_alpha = texture.a * fragColor.a
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_VCA,     ID_LCOL, 3)
    s.emit(OP_F_MUL,             ID_FLOAT, ID_FINAL_A, ID_SA,   ID_VCA)

    # Extract RGB from vertex color
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_VCR, ID_LCOL, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_VCG, ID_LCOL, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_VCB, ID_LCOL, 2)

    # outColor = vec4(fragColor.rgb, final_alpha)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_RESULT,
           ID_VCR, ID_VCG, ID_VCB, ID_FINAL_A)
    s.emit(OP_STORE, ID_OUT_COL, ID_RESULT)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


# ── Item vertex shader ────────────────────────────────────────────────────────
def build_item_vertex():
    """
    GLSL equivalent:
        layout(location=0) in  vec3  inPos;
        layout(location=1) in  vec4  inColor;
        layout(location=2) in  vec2  inUV;
        layout(location=3) in  float inTexIdx;
        layout(location=0) out vec4  fragColor;
        layout(location=1) out vec2  fragUV;
        layout(location=2) out float fragTexIdx;
        layout(set=1, binding=0) uniform UBO { mat4 mvp; } ubo;
        out gl_PerVertex { vec4 gl_Position; };
        void main() {
            gl_Position = ubo.mvp * vec4(inPos, 1.0);
            fragColor   = inColor;
            fragUV      = inUV;
            fragTexIdx  = inTexIdx;
        }
    """
    s = Spirv()
    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC3         = s.new()
    ID_VEC4         = s.new()
    ID_MAT4         = s.new()
    ID_INT          = s.new()
    ID_PV_STRUCT    = s.new()
    ID_PC_STRUCT    = s.new()
    ID_PTR_OUT_PV   = s.new()
    ID_PTR_PC_S     = s.new()
    ID_PTR_IN_V3    = s.new()
    ID_PTR_IN_V4    = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_IN_F     = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_PTR_OUT_V2   = s.new()
    ID_PTR_OUT_F    = s.new()
    ID_PTR_PC_M4    = s.new()
    ID_GL_POS_BLOCK = s.new()
    ID_PC_VAR       = s.new()
    ID_IN_POS       = s.new()
    ID_IN_COLOR     = s.new()
    ID_IN_UV        = s.new()
    ID_IN_TEXIDX    = s.new()
    ID_OUT_COLOR    = s.new()
    ID_OUT_UV       = s.new()
    ID_OUT_TEXIDX   = s.new()
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
    ID_LCOL         = s.new()
    ID_LUV          = s.new()
    ID_LTIDX        = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_VERTEX, ID_MAIN, "main",
                  ID_IN_POS, ID_IN_COLOR, ID_IN_UV, ID_IN_TEXIDX,
                  ID_GL_POS_BLOCK, ID_OUT_COLOR, ID_OUT_UV, ID_OUT_TEXIDX)

    s.emit(OP_DECORATE, ID_IN_POS,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_IN_COLOR,   DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_IN_UV,      DEC_LOCATION, 2)
    s.emit(OP_DECORATE, ID_IN_TEXIDX,  DEC_LOCATION, 3)
    s.emit(OP_DECORATE, ID_OUT_COLOR,  DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_OUT_UV,     DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_OUT_TEXIDX, DEC_LOCATION, 2)
    s.emit(OP_DECORATE,        ID_PV_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PV_STRUCT, 0, DEC_BUILT_IN, BI_POSITION)
    s.emit(OP_DECORATE,        ID_PC_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_COL_MAJOR)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_MATRIX_STRIDE, 16)
    s.emit(OP_MEMBER_DECORATE, ID_PC_STRUCT, 0, DEC_OFFSET, 0)
    s.emit(OP_DECORATE, ID_PC_VAR, DEC_DESCRIPTOR_SET, 1)
    s.emit(OP_DECORATE, ID_PC_VAR, DEC_BINDING,        0)

    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,   ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,   ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_MATRIX,   ID_MAT4, ID_VEC4, 4)
    s.emit(OP_TYPE_INT,      ID_INT, 32, 1)
    s.emit(OP_TYPE_STRUCT,   ID_PV_STRUCT, ID_VEC4)
    s.emit(OP_TYPE_STRUCT,   ID_PC_STRUCT, ID_MAT4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_PV, SC_OUTPUT,  ID_PV_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_PC_S,   SC_UNIFORM, ID_PC_STRUCT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V3,  SC_INPUT,   ID_VEC3)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V4,  SC_INPUT,   ID_VEC4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_V2,  SC_INPUT,   ID_VEC2)
    s.emit(OP_TYPE_POINTER,  ID_PTR_IN_F,   SC_INPUT,   ID_FLOAT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V4, SC_OUTPUT,  ID_VEC4)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_V2, SC_OUTPUT,  ID_VEC2)
    s.emit(OP_TYPE_POINTER,  ID_PTR_OUT_F,  SC_OUTPUT,  ID_FLOAT)
    s.emit(OP_TYPE_POINTER,  ID_PTR_PC_M4,  SC_UNIFORM, ID_MAT4)

    s.emit(OP_VARIABLE, ID_PTR_OUT_PV, ID_GL_POS_BLOCK, SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_PC_S,   ID_PC_VAR,       SC_UNIFORM)
    s.emit(OP_VARIABLE, ID_PTR_IN_V3,  ID_IN_POS,       SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V4,  ID_IN_COLOR,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,  ID_IN_UV,        SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_F,   ID_IN_TEXIDX,    SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4, ID_OUT_COLOR,    SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V2, ID_OUT_UV,       SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_F,  ID_OUT_TEXIDX,   SC_OUTPUT)
    s.emit(OP_CONSTANT, ID_INT,   ID_CI0, 0)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CF1, f2w(1.0))

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)
    # Transform pos to clip space
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
    # Pass through color
    s.emit(OP_LOAD,  ID_VEC4,  ID_LCOL,  ID_IN_COLOR)
    s.emit(OP_STORE, ID_OUT_COLOR,  ID_LCOL)
    # Pass through UV
    s.emit(OP_LOAD,  ID_VEC2,  ID_LUV,   ID_IN_UV)
    s.emit(OP_STORE, ID_OUT_UV, ID_LUV)
    # Pass through texIdx
    s.emit(OP_LOAD,  ID_FLOAT, ID_LTIDX, ID_IN_TEXIDX)
    s.emit(OP_STORE, ID_OUT_TEXIDX, ID_LTIDX)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


# ── Item fragment shader ───────────────────────────────────────────────────────
def build_item_fragment():
    """
    GLSL equivalent:
        layout(location=0) in  vec4  fragColor;
        layout(location=1) in  vec2  fragUV;
        layout(location=2) in  float fragTexIdx;
        layout(set=2, binding=0) uniform sampler2DArray tex;
        layout(location=0) out vec4 outColor;
        void main() {
            vec4 tc = texture(tex, vec3(fragUV.x, fragUV.y, fragTexIdx));
            outColor = tc * fragColor;
        }
    """
    s = Spirv()
    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC3         = s.new()
    ID_VEC4         = s.new()
    ID_IMAGE        = s.new()
    ID_SAMP_IMG     = s.new()
    ID_PTR_UC_SIMG  = s.new()
    ID_PTR_IN_V4    = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_IN_F     = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_SAMPLER_VAR  = s.new()
    ID_IN_COLOR     = s.new()
    ID_IN_UV        = s.new()
    ID_IN_TEXIDX    = s.new()
    ID_OUT_COL      = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    ID_LCOL         = s.new()
    ID_LUV          = s.new()
    ID_LTIDX        = s.new()
    ID_UVX          = s.new()
    ID_UVY          = s.new()
    ID_UVW          = s.new()
    ID_SIMG         = s.new()
    ID_TC           = s.new()
    ID_OUT_VAL      = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main",
                  ID_IN_COLOR, ID_IN_UV, ID_IN_TEXIDX, ID_OUT_COL)
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    s.emit(OP_DECORATE, ID_IN_COLOR,    DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_IN_UV,       DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_IN_TEXIDX,   DEC_LOCATION, 2)
    s.emit(OP_DECORATE, ID_OUT_COL,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_BINDING,        0)

    s.emit(OP_TYPE_VOID,     ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,    ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,   ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,   ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR,   ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_IMAGE,         ID_IMAGE,    ID_FLOAT, 1, 0, 1, 0, 1, 0)
    s.emit(OP_TYPE_SAMPLED_IMAGE, ID_SAMP_IMG, ID_IMAGE)
    s.emit(OP_TYPE_POINTER, ID_PTR_UC_SIMG, SC_UNIFORM_CONSTANT, ID_SAMP_IMG)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V4,   SC_INPUT,            ID_VEC4)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,   SC_INPUT,            ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_F,    SC_INPUT,            ID_FLOAT)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4,  SC_OUTPUT,           ID_VEC4)

    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_SAMPLER_VAR, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V4,   ID_IN_COLOR,  SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_IN_UV,     SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_F,    ID_IN_TEXIDX, SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4,  ID_OUT_COL,   SC_OUTPUT)

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL, ID_ENTRY_LBL)
    s.emit(OP_LOAD, ID_VEC4,  ID_LCOL,  ID_IN_COLOR)
    s.emit(OP_LOAD, ID_VEC2,  ID_LUV,   ID_IN_UV)
    s.emit(OP_LOAD, ID_FLOAT, ID_LTIDX, ID_IN_TEXIDX)
    s.emit(OP_COMPOSITE_EXTRACT,   ID_FLOAT, ID_UVX, ID_LUV, 0)
    s.emit(OP_COMPOSITE_EXTRACT,   ID_FLOAT, ID_UVY, ID_LUV, 1)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC3,  ID_UVW, ID_UVX, ID_UVY, ID_LTIDX)
    s.emit(OP_LOAD,  ID_SAMP_IMG, ID_SIMG, ID_SAMPLER_VAR)
    s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, ID_TC, ID_SIMG, ID_UVW)
    s.emit(OP_F_MUL, ID_VEC4, ID_OUT_VAL, ID_TC, ID_LCOL)
    s.emit(OP_STORE, ID_OUT_COL, ID_OUT_VAL)
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


# ── Bloom: fullscreen vertex shader ───────────────────────────────────────────
def build_bloom_vertex():
    """
    GLSL equivalent:
        layout(location=0) in  vec2 inPos;   // NDC position
        layout(location=1) in  vec2 inUV;
        layout(location=0) out vec2 fragUV;
        out gl_PerVertex { vec4 gl_Position; };
        void main() {
            gl_Position = vec4(inPos.x, inPos.y, 0.0, 1.0);
            fragUV = inUV;
        }
    """
    s = Spirv()

    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC4         = s.new()
    ID_INT          = s.new()
    ID_PV_STRUCT    = s.new()
    ID_PTR_OUT_PV   = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_OUT_V2   = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_GL_POS_BLOCK = s.new()
    ID_IN_POS       = s.new()
    ID_IN_UV        = s.new()
    ID_OUT_UV       = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    ID_CI0          = s.new()
    ID_CF0          = s.new()
    ID_CF1          = s.new()
    ID_LPOS         = s.new()
    ID_PX           = s.new()
    ID_PY           = s.new()
    ID_POSV4        = s.new()
    ID_P_GPOS       = s.new()
    ID_LUV          = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_VERTEX, ID_MAIN, "main",
                  ID_IN_POS, ID_IN_UV, ID_GL_POS_BLOCK, ID_OUT_UV)

    s.emit(OP_DECORATE, ID_IN_POS,  DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_IN_UV,   DEC_LOCATION, 1)
    s.emit(OP_DECORATE, ID_OUT_UV,  DEC_LOCATION, 0)
    s.emit(OP_DECORATE,        ID_PV_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_PV_STRUCT, 0, DEC_BUILT_IN, BI_POSITION)

    s.emit(OP_TYPE_VOID,    ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,   ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,  ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,  ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_INT,     ID_INT, 32, 1)
    s.emit(OP_TYPE_STRUCT,  ID_PV_STRUCT, ID_VEC4)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_PV, SC_OUTPUT, ID_PV_STRUCT)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,  SC_INPUT,  ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V2, SC_OUTPUT, ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4, SC_OUTPUT, ID_VEC4)

    s.emit(OP_VARIABLE, ID_PTR_OUT_PV, ID_GL_POS_BLOCK, SC_OUTPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,  ID_IN_POS,       SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,  ID_IN_UV,        SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V2, ID_OUT_UV,       SC_OUTPUT)

    s.emit(OP_CONSTANT, ID_INT,   ID_CI0, 0)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CF0, f2w(0.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CF1, f2w(1.0))

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL,    ID_ENTRY_LBL)

    s.emit(OP_LOAD, ID_VEC2, ID_LPOS, ID_IN_POS)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_PX, ID_LPOS, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_PY, ID_LPOS, 1)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_POSV4, ID_PX, ID_PY, ID_CF0, ID_CF1)
    s.emit(OP_ACCESS_CHAIN, ID_PTR_OUT_V4, ID_P_GPOS, ID_GL_POS_BLOCK, ID_CI0)
    s.emit(OP_STORE, ID_P_GPOS, ID_POSV4)

    s.emit(OP_LOAD,  ID_VEC2, ID_LUV, ID_IN_UV)
    s.emit(OP_STORE, ID_OUT_UV, ID_LUV)

    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


# ── Bloom: brightness-threshold extract fragment ───────────────────────────────
def build_bloom_thresh_frag():
    """
    GLSL equivalent:
        layout(set=2, binding=0) uniform sampler2D sceneTex;
        layout(location=0) in  vec2 fragUV;
        layout(location=0) out vec4 outColor;
        void main() {
            vec4 tc = texture(sceneTex, fragUV);
            float lum = tc.r*0.2126 + tc.g*0.7152 + tc.b*0.0722;
            float excess = max(lum - 0.75, 0.0);
            float scale  = excess / max(lum, 0.0001);
            outColor = vec4(tc.rgb * scale, 1.0);
        }
    """
    GLSL_FMAX = 40

    s = Spirv()
    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC4         = s.new()
    ID_IMAGE        = s.new()
    ID_SAMP_IMG     = s.new()
    ID_PTR_UC_SIMG  = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_SAMPLER_VAR  = s.new()
    ID_IN_UV        = s.new()
    ID_OUT_COL      = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    # float constants
    ID_C0           = s.new()
    ID_C1           = s.new()
    ID_C2126        = s.new()
    ID_C7152        = s.new()
    ID_C0722        = s.new()
    ID_C_KNEE       = s.new()
    ID_C_SMALL      = s.new()
    # runtime
    ID_LUV          = s.new()
    ID_SIMG         = s.new()
    ID_TC           = s.new()
    ID_TC_R         = s.new()
    ID_TC_G         = s.new()
    ID_TC_B         = s.new()
    ID_LUM_R        = s.new()
    ID_LUM_G        = s.new()
    ID_LUM_B        = s.new()
    ID_LUM_RG       = s.new()
    ID_LUM          = s.new()
    ID_LUM_MINUS    = s.new()
    ID_EXCESS       = s.new()
    ID_LUM_CLAMPED  = s.new()
    ID_SCALE        = s.new()
    ID_OUT_R        = s.new()
    ID_OUT_G        = s.new()
    ID_OUT_B        = s.new()
    ID_OUT_COLOR    = s.new()
    ID_GLSL_EXT     = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    OP_EXT_INST_IMPORT = 11
    ext_words = str_words("GLSL.std.450")
    wc = 1 + 1 + len(ext_words)
    s.words.append(wc << 16 | OP_EXT_INST_IMPORT)
    s.words.append(ID_GLSL_EXT)
    s.words.extend(ext_words)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main", ID_IN_UV, ID_OUT_COL)
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    s.emit(OP_DECORATE, ID_IN_UV,       DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_OUT_COL,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_BINDING, 0)

    s.emit(OP_TYPE_VOID,    ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,   ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,  ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,  ID_VEC4, ID_FLOAT, 4)
    # sampler2D (non-array: arrayed=0)
    s.emit(OP_TYPE_IMAGE,         ID_IMAGE,   ID_FLOAT, 1, 0, 0, 0, 1, 0)
    s.emit(OP_TYPE_SAMPLED_IMAGE, ID_SAMP_IMG, ID_IMAGE)
    s.emit(OP_TYPE_POINTER, ID_PTR_UC_SIMG, SC_UNIFORM_CONSTANT, ID_SAMP_IMG)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,   SC_INPUT,            ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4,  SC_OUTPUT,           ID_VEC4)

    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_SAMPLER_VAR, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_IN_UV,       SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4,  ID_OUT_COL,     SC_OUTPUT)

    s.emit(OP_CONSTANT, ID_FLOAT, ID_C0,     f2w(0.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C1,     f2w(1.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C2126,  f2w(0.2126))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C7152,  f2w(0.7152))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C0722,  f2w(0.0722))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C_KNEE, f2w(0.75))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C_SMALL,f2w(0.0001))

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL,    ID_ENTRY_LBL)

    s.emit(OP_LOAD, ID_VEC2, ID_LUV, ID_IN_UV)
    s.emit(OP_LOAD, ID_SAMP_IMG, ID_SIMG, ID_SAMPLER_VAR)
    s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, ID_TC, ID_SIMG, ID_LUV)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_R, ID_TC, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_G, ID_TC, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_TC_B, ID_TC, 2)

    # lum = r*0.2126 + g*0.7152 + b*0.0722
    s.emit(OP_F_MUL, ID_FLOAT, ID_LUM_R,  ID_TC_R, ID_C2126)
    s.emit(OP_F_MUL, ID_FLOAT, ID_LUM_G,  ID_TC_G, ID_C7152)
    s.emit(OP_F_MUL, ID_FLOAT, ID_LUM_B,  ID_TC_B, ID_C0722)
    s.emit(OP_F_ADD, ID_FLOAT, ID_LUM_RG, ID_LUM_R, ID_LUM_G)
    s.emit(OP_F_ADD, ID_FLOAT, ID_LUM,    ID_LUM_RG, ID_LUM_B)

    # excess = max(lum - 0.75, 0.0)
    s.emit(OP_F_SUB, ID_FLOAT, ID_LUM_MINUS, ID_LUM, ID_C_KNEE)
    s.emit(OP_EXT_INST, ID_FLOAT, ID_EXCESS, ID_GLSL_EXT, GLSL_FMAX, ID_LUM_MINUS, ID_C0)

    # scale = excess / max(lum, 0.0001)
    s.emit(OP_EXT_INST, ID_FLOAT, ID_LUM_CLAMPED, ID_GLSL_EXT, GLSL_FMAX, ID_LUM, ID_C_SMALL)
    s.emit(OP_F_DIV, ID_FLOAT, ID_SCALE, ID_EXCESS, ID_LUM_CLAMPED)

    # out = tc.rgb * scale
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_R, ID_TC_R, ID_SCALE)
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_G, ID_TC_G, ID_SCALE)
    s.emit(OP_F_MUL, ID_FLOAT, ID_OUT_B, ID_TC_B, ID_SCALE)
    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_OUT_COLOR,
           ID_OUT_R, ID_OUT_G, ID_OUT_B, ID_C1)
    s.emit(OP_STORE, ID_OUT_COL, ID_OUT_COLOR)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


# ── Bloom: separable Gaussian blur fragment (5-tap, direction via UBO) ─────────
def build_bloom_blur_frag():
    """
    GLSL equivalent (direction supplied via UBO vec4.xy = per-pixel step):
        layout(set=2, binding=0) uniform sampler2D srcTex;
        layout(set=3, binding=0) uniform BlurUBO { vec4 params; }; // xy = step dir
        layout(location=0) in  vec2 fragUV;
        layout(location=0) out vec4 outColor;
        const float W[5] = { 0.0625, 0.25, 0.375, 0.25, 0.0625 };
        void main() {
            vec2 dir = vec2(params.x, params.y);
            vec3 acc = vec3(0.0);
            acc += texture(srcTex, fragUV + dir * -2.0).rgb * W[0];
            acc += texture(srcTex, fragUV + dir * -1.0).rgb * W[1];
            acc += texture(srcTex, fragUV           ).rgb * W[2];
            acc += texture(srcTex, fragUV + dir *  1.0).rgb * W[3];
            acc += texture(srcTex, fragUV + dir *  2.0).rgb * W[4];
            outColor = vec4(acc, 1.0);
        }
    """
    GLSL_FMAX = 40   # (unused here, kept for consistency)

    s = Spirv()
    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC3         = s.new()
    ID_VEC4         = s.new()
    ID_INT          = s.new()
    ID_IMAGE        = s.new()
    ID_SAMP_IMG     = s.new()
    ID_PTR_UC_SIMG  = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_OUT_V4   = s.new()
    # UBO struct { vec4 params }
    ID_UBO_STRUCT   = s.new()
    ID_PTR_UNI_UBO  = s.new()
    ID_PTR_UNI_V4   = s.new()
    ID_SAMPLER_VAR  = s.new()
    ID_UBO_VAR      = s.new()
    ID_IN_UV        = s.new()
    ID_OUT_COL      = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    # constants
    ID_CI0          = s.new()
    ID_C0           = s.new()
    ID_C1           = s.new()
    ID_CN2          = s.new()   # -2.0
    ID_CN1          = s.new()   # -1.0
    ID_CP1          = s.new()   #  1.0
    ID_CP2          = s.new()   #  2.0
    ID_W0           = s.new()   # 0.0625
    ID_W1           = s.new()   # 0.25
    ID_W2           = s.new()   # 0.375
    ID_W3           = s.new()   # 0.25
    ID_W4           = s.new()   # 0.0625
    # runtime
    ID_LUV          = s.new()
    ID_UV_X         = s.new()
    ID_UV_Y         = s.new()
    ID_SIMG         = s.new()
    ID_PARAMS       = s.new()
    ID_P_PARAMS     = s.new()
    ID_DIR_X        = s.new()
    ID_DIR_Y        = s.new()
    # accumulators (R, G, B for final result)
    ID_ACC_R        = s.new()
    ID_ACC_G        = s.new()
    ID_ACC_B        = s.new()
    ID_OUT_COLOR    = s.new()
    # per-tap temporaries (5 taps × 8 IDs each)
    tap_ids = [[s.new() for _ in range(10)] for _ in range(5)]

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main", ID_IN_UV, ID_OUT_COL)
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    s.emit(OP_DECORATE, ID_IN_UV,       DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_OUT_COL,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_SAMPLER_VAR, DEC_BINDING, 0)
    s.emit(OP_DECORATE,        ID_UBO_STRUCT, DEC_BLOCK)
    s.emit(OP_MEMBER_DECORATE, ID_UBO_STRUCT, 0, DEC_OFFSET, 0)
    s.emit(OP_DECORATE, ID_UBO_VAR, DEC_DESCRIPTOR_SET, 3)
    s.emit(OP_DECORATE, ID_UBO_VAR, DEC_BINDING, 0)

    s.emit(OP_TYPE_VOID,    ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,   ID_FLOAT, 32)
    s.emit(OP_TYPE_INT,     ID_INT, 32, 1)
    s.emit(OP_TYPE_VECTOR,  ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,  ID_VEC3, ID_FLOAT, 3)
    s.emit(OP_TYPE_VECTOR,  ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_IMAGE,         ID_IMAGE,    ID_FLOAT, 1, 0, 0, 0, 1, 0)
    s.emit(OP_TYPE_SAMPLED_IMAGE, ID_SAMP_IMG, ID_IMAGE)
    s.emit(OP_TYPE_POINTER, ID_PTR_UC_SIMG, SC_UNIFORM_CONSTANT, ID_SAMP_IMG)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,   SC_INPUT,  ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4,  SC_OUTPUT, ID_VEC4)
    s.emit(OP_TYPE_STRUCT,  ID_UBO_STRUCT, ID_VEC4)
    s.emit(OP_TYPE_POINTER, ID_PTR_UNI_UBO, SC_UNIFORM, ID_UBO_STRUCT)
    s.emit(OP_TYPE_POINTER, ID_PTR_UNI_V4,  SC_UNIFORM, ID_VEC4)

    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_SAMPLER_VAR, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_UNI_UBO, ID_UBO_VAR,     SC_UNIFORM)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_IN_UV,       SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4,  ID_OUT_COL,     SC_OUTPUT)

    s.emit(OP_CONSTANT, ID_INT,   ID_CI0, 0)
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C0,  f2w(0.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_C1,  f2w(1.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CN2, f2w(-2.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CN1, f2w(-1.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CP1, f2w( 1.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_CP2, f2w( 2.0))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_W0,  f2w(0.0625))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_W1,  f2w(0.25))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_W2,  f2w(0.375))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_W3,  f2w(0.25))
    s.emit(OP_CONSTANT, ID_FLOAT, ID_W4,  f2w(0.0625))

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL,    ID_ENTRY_LBL)

    # Load UV, sampler, UBO
    s.emit(OP_LOAD, ID_VEC2, ID_LUV, ID_IN_UV)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_UV_X, ID_LUV, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_UV_Y, ID_LUV, 1)
    s.emit(OP_LOAD, ID_SAMP_IMG, ID_SIMG, ID_SAMPLER_VAR)
    s.emit(OP_ACCESS_CHAIN, ID_PTR_UNI_V4, ID_P_PARAMS, ID_UBO_VAR, ID_CI0)
    s.emit(OP_LOAD, ID_VEC4, ID_PARAMS, ID_P_PARAMS)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_DIR_X, ID_PARAMS, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_DIR_Y, ID_PARAMS, 1)

    # Helper: emit one tap and return (r,g,b) IDs
    # offsets and weights for taps 0..4
    offsets  = [ID_CN2, ID_CN1, ID_C0, ID_CP1, ID_CP2]
    weights  = [ID_W0,  ID_W1,  ID_W2, ID_W3,  ID_W4]
    tap_r, tap_g, tap_b = [], [], []

    for i, (off_id, w_id) in enumerate(zip(offsets, weights)):
        t = tap_ids[i]
        # sampleX = uvX + dirX * offset
        s.emit(OP_F_MUL, ID_FLOAT, t[0], ID_DIR_X, off_id)
        s.emit(OP_F_MUL, ID_FLOAT, t[1], ID_DIR_Y, off_id)
        s.emit(OP_F_ADD, ID_FLOAT, t[2], ID_UV_X, t[0])
        s.emit(OP_F_ADD, ID_FLOAT, t[3], ID_UV_Y, t[1])
        s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC2, t[4], t[2], t[3])
        s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, t[5], ID_SIMG, t[4])
        s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, t[6], t[5], 0)   # R
        s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, t[7], t[5], 1)   # G
        s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, t[8], t[5], 2)   # B
        # weighted: wR = R * weight
        wR = s.new(); wG = s.new(); wB = s.new()
        s.emit(OP_F_MUL, ID_FLOAT, wR, t[6], w_id)
        s.emit(OP_F_MUL, ID_FLOAT, wG, t[7], w_id)
        s.emit(OP_F_MUL, ID_FLOAT, wB, t[8], w_id)
        tap_r.append(wR); tap_g.append(wG); tap_b.append(wB)

    # Accumulate: acc = tap0 + tap1 + tap2 + tap3 + tap4
    def fadd_chain(ids):
        result = ids[0]
        for x in ids[1:]:
            tmp = s.new()
            s.emit(OP_F_ADD, ID_FLOAT, tmp, result, x)
            result = tmp
        return result

    acc_r = fadd_chain(tap_r)
    acc_g = fadd_chain(tap_g)
    acc_b = fadd_chain(tap_b)

    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_OUT_COLOR, acc_r, acc_g, acc_b, ID_C1)
    s.emit(OP_STORE, ID_OUT_COL, ID_OUT_COLOR)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


# ── Bloom: composite (scene + bloom) fragment ─────────────────────────────────
def build_bloom_comp_frag():
    """
    GLSL equivalent:
        layout(set=2, binding=0) uniform sampler2D sceneTex;
        layout(set=2, binding=1) uniform sampler2D bloomTex;
        layout(location=0) in  vec2 fragUV;
        layout(location=0) out vec4 outColor;
        void main() {
            vec4 scene = texture(sceneTex, fragUV);
            vec3 bloom = texture(bloomTex, fragUV).rgb;
            outColor = vec4(scene.rgb + bloom * 0.5, scene.a);
        }
    """
    s = Spirv()

    ID_VOID         = s.new()
    ID_FN_VT        = s.new()
    ID_FLOAT        = s.new()
    ID_VEC2         = s.new()
    ID_VEC4         = s.new()
    ID_IMAGE        = s.new()
    ID_SAMP_IMG     = s.new()
    ID_PTR_UC_SIMG  = s.new()
    ID_PTR_IN_V2    = s.new()
    ID_PTR_OUT_V4   = s.new()
    ID_SCENE_SAMP   = s.new()   # set=2, binding=0
    ID_BLOOM_SAMP   = s.new()   # set=2, binding=1
    ID_IN_UV        = s.new()
    ID_OUT_COL      = s.new()
    ID_MAIN         = s.new()
    ID_ENTRY_LBL    = s.new()
    # constants
    ID_C_BLOOM_STR  = s.new()   # 0.5 (bloom strength multiplier)
    # runtime
    ID_LUV          = s.new()
    ID_SIMG_SCENE   = s.new()
    ID_SIMG_BLOOM   = s.new()
    ID_SC_TC        = s.new()
    ID_BL_TC        = s.new()
    ID_SC_R         = s.new()
    ID_SC_G         = s.new()
    ID_SC_B         = s.new()
    ID_SC_A         = s.new()
    ID_BL_R         = s.new()
    ID_BL_G         = s.new()
    ID_BL_B         = s.new()
    ID_BL_SR        = s.new()
    ID_BL_SG        = s.new()
    ID_BL_SB        = s.new()
    ID_OUT_R        = s.new()
    ID_OUT_G        = s.new()
    ID_OUT_B        = s.new()
    ID_OUT_COLOR    = s.new()

    s.emit(OP_CAPABILITY, CAP_SHADER)
    s.emit(OP_MEMORY_MODEL, ADR_LOGICAL, MEM_GLSL450)
    s.entry_point(EXM_FRAGMENT, ID_MAIN, "main", ID_IN_UV, ID_OUT_COL)
    s.emit(OP_EXECUTION_MODE, ID_MAIN, EM_ORIGIN_UPPER_LEFT)

    s.emit(OP_DECORATE, ID_IN_UV,       DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_OUT_COL,     DEC_LOCATION, 0)
    s.emit(OP_DECORATE, ID_SCENE_SAMP,  DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_SCENE_SAMP,  DEC_BINDING, 0)
    s.emit(OP_DECORATE, ID_BLOOM_SAMP,  DEC_DESCRIPTOR_SET, 2)
    s.emit(OP_DECORATE, ID_BLOOM_SAMP,  DEC_BINDING, 1)

    s.emit(OP_TYPE_VOID,    ID_VOID)
    s.emit(OP_TYPE_FUNCTION, ID_FN_VT, ID_VOID)
    s.emit(OP_TYPE_FLOAT,   ID_FLOAT, 32)
    s.emit(OP_TYPE_VECTOR,  ID_VEC2, ID_FLOAT, 2)
    s.emit(OP_TYPE_VECTOR,  ID_VEC4, ID_FLOAT, 4)
    s.emit(OP_TYPE_IMAGE,         ID_IMAGE,    ID_FLOAT, 1, 0, 0, 0, 1, 0)
    s.emit(OP_TYPE_SAMPLED_IMAGE, ID_SAMP_IMG, ID_IMAGE)
    s.emit(OP_TYPE_POINTER, ID_PTR_UC_SIMG, SC_UNIFORM_CONSTANT, ID_SAMP_IMG)
    s.emit(OP_TYPE_POINTER, ID_PTR_IN_V2,   SC_INPUT,  ID_VEC2)
    s.emit(OP_TYPE_POINTER, ID_PTR_OUT_V4,  SC_OUTPUT, ID_VEC4)

    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_SCENE_SAMP, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_UC_SIMG, ID_BLOOM_SAMP, SC_UNIFORM_CONSTANT)
    s.emit(OP_VARIABLE, ID_PTR_IN_V2,   ID_IN_UV,      SC_INPUT)
    s.emit(OP_VARIABLE, ID_PTR_OUT_V4,  ID_OUT_COL,    SC_OUTPUT)

    s.emit(OP_CONSTANT, ID_FLOAT, ID_C_BLOOM_STR, f2w(0.5))

    s.emit(OP_FUNCTION, ID_VOID, ID_MAIN, FC_NONE, ID_FN_VT)
    s.emit(OP_LABEL,    ID_ENTRY_LBL)

    s.emit(OP_LOAD, ID_VEC2, ID_LUV, ID_IN_UV)

    # Sample scene
    s.emit(OP_LOAD, ID_SAMP_IMG, ID_SIMG_SCENE, ID_SCENE_SAMP)
    s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, ID_SC_TC, ID_SIMG_SCENE, ID_LUV)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_SC_R, ID_SC_TC, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_SC_G, ID_SC_TC, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_SC_B, ID_SC_TC, 2)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_SC_A, ID_SC_TC, 3)

    # Sample bloom
    s.emit(OP_LOAD, ID_SAMP_IMG, ID_SIMG_BLOOM, ID_BLOOM_SAMP)
    s.emit(OP_IMAGE_SAMPLE_IMPLICIT_LOD, ID_VEC4, ID_BL_TC, ID_SIMG_BLOOM, ID_LUV)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_BL_R, ID_BL_TC, 0)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_BL_G, ID_BL_TC, 1)
    s.emit(OP_COMPOSITE_EXTRACT, ID_FLOAT, ID_BL_B, ID_BL_TC, 2)

    # bloom * strength
    s.emit(OP_F_MUL, ID_FLOAT, ID_BL_SR, ID_BL_R, ID_C_BLOOM_STR)
    s.emit(OP_F_MUL, ID_FLOAT, ID_BL_SG, ID_BL_G, ID_C_BLOOM_STR)
    s.emit(OP_F_MUL, ID_FLOAT, ID_BL_SB, ID_BL_B, ID_C_BLOOM_STR)

    # scene + bloom*strength
    s.emit(OP_F_ADD, ID_FLOAT, ID_OUT_R, ID_SC_R, ID_BL_SR)
    s.emit(OP_F_ADD, ID_FLOAT, ID_OUT_G, ID_SC_G, ID_BL_SG)
    s.emit(OP_F_ADD, ID_FLOAT, ID_OUT_B, ID_SC_B, ID_BL_SB)

    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC4, ID_OUT_COLOR,
           ID_OUT_R, ID_OUT_G, ID_OUT_B, ID_SC_A)
    s.emit(OP_STORE, ID_OUT_COL, ID_OUT_COLOR)
    s.emit(OP_RETURN)
    s.emit(OP_FUNCTION_END)
    return s.build()


if __name__ == '__main__':
    root = pathlib.Path(__file__).parent.parent
    out  = root / 'src' / 'render' / 'shaders'
    out.mkdir(parents=True, exist_ok=True)

    vert_spv    = build_vertex()
    frag_spv    = build_fragment()
    hl_vert_spv = build_highlight_vertex()
    hl_frag_spv = build_highlight_fragment()
    ui_vert_spv      = build_ui_vertex()
    ui_frag_spv      = build_ui_fragment()
    ui_text_frag_spv = build_mtsdf_fragment()
    item_vert_spv = build_item_vertex()
    item_frag_spv = build_item_fragment()
    bloom_vert_spv       = build_bloom_vertex()
    bloom_thresh_frag_spv = build_bloom_thresh_frag()
    bloom_blur_frag_spv  = build_bloom_blur_frag()
    bloom_comp_frag_spv  = build_bloom_comp_frag()

    (out / 'chunk_vert_spv.h'       ).write_text(to_cpp_header('k_chunk_vert_spv',      vert_spv))
    (out / 'chunk_frag_spv.h'       ).write_text(to_cpp_header('k_chunk_frag_spv',      frag_spv))
    (out / 'highlight_vert_spv.h'   ).write_text(to_cpp_header('k_highlight_vert_spv',  hl_vert_spv))
    (out / 'highlight_frag_spv.h'   ).write_text(to_cpp_header('k_highlight_frag_spv',  hl_frag_spv))
    (out / 'ui_vert_spv.h'          ).write_text(to_cpp_header('k_ui_vert_spv',         ui_vert_spv))
    (out / 'ui_frag_spv.h'          ).write_text(to_cpp_header('k_ui_frag_spv',         ui_frag_spv))
    (out / 'ui_text_frag_spv.h'     ).write_text(to_cpp_header('k_ui_text_frag_spv',    ui_text_frag_spv))
    (out / 'item_vert_spv.h'        ).write_text(to_cpp_header('k_item_vert_spv',       item_vert_spv))
    (out / 'item_frag_spv.h'        ).write_text(to_cpp_header('k_item_frag_spv',       item_frag_spv))
    (out / 'bloom_vert_spv.h'       ).write_text(to_cpp_header('k_bloom_vert_spv',      bloom_vert_spv))
    (out / 'bloom_thresh_frag_spv.h').write_text(to_cpp_header('k_bloom_thresh_frag_spv', bloom_thresh_frag_spv))
    (out / 'bloom_blur_frag_spv.h'  ).write_text(to_cpp_header('k_bloom_blur_frag_spv', bloom_blur_frag_spv))
    (out / 'bloom_comp_frag_spv.h'  ).write_text(to_cpp_header('k_bloom_comp_frag_spv', bloom_comp_frag_spv))

    print(f'chunk_vert_spv.h       : {len(vert_spv)} bytes  ({len(vert_spv)//4} words)')
    print(f'chunk_frag_spv.h       : {len(frag_spv)} bytes  ({len(frag_spv)//4} words)')
    print(f'highlight_vert_spv.h   : {len(hl_vert_spv)} bytes  ({len(hl_vert_spv)//4} words)')
    print(f'highlight_frag_spv.h   : {len(hl_frag_spv)} bytes  ({len(hl_frag_spv)//4} words)')
    print(f'ui_vert_spv.h          : {len(ui_vert_spv)} bytes  ({len(ui_vert_spv)//4} words)')
    print(f'ui_frag_spv.h          : {len(ui_frag_spv)} bytes  ({len(ui_frag_spv)//4} words)')
    print(f'ui_text_frag_spv.h     : {len(ui_text_frag_spv)} bytes  ({len(ui_text_frag_spv)//4} words)')
    print(f'item_vert_spv.h        : {len(item_vert_spv)} bytes  ({len(item_vert_spv)//4} words)')
    print(f'item_frag_spv.h        : {len(item_frag_spv)} bytes  ({len(item_frag_spv)//4} words)')
    print(f'bloom_vert_spv.h           : {len(bloom_vert_spv)} bytes  ({len(bloom_vert_spv)//4} words)')
    print(f'bloom_thresh_frag_spv.h    : {len(bloom_thresh_frag_spv)} bytes  ({len(bloom_thresh_frag_spv)//4} words)')
    print(f'bloom_blur_frag_spv.h      : {len(bloom_blur_frag_spv)} bytes  ({len(bloom_blur_frag_spv)//4} words)')
    print(f'bloom_comp_frag_spv.h      : {len(bloom_comp_frag_spv)} bytes  ({len(bloom_comp_frag_spv)//4} words)')
    print('Done.')
