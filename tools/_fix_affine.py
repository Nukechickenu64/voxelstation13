"""
Fix affine texture mapping to be visually obvious.

The NoPerspective UV warp is too subtle on small 1x1 voxel faces because the W
variation across a single tile is small (~10-30%). We add UV quantization to the
fragment shader: snap UV to a 32-step grid blended by affine_mix. This produces
the classic PS1 "stepped / swimming" texture look that is always clearly visible.

The NoPerspective interpolation stays and still contributes the actual warp; the
quantization makes the effect obviously perceptible at any camera angle.
"""
import sys

with open('tools/gen_shaders.py', 'r', encoding='utf-8') as f:
    src = f.read()

# ─────────────────────────────────────────────────────────────────────────────
# 1. Add new temp IDs to build_fragment() just after the existing affine IDs
# ─────────────────────────────────────────────────────────────────────────────
OLD_IDS = (
    '    ID_MIX_UX        = s.new()   # FMix result x\n'
    '    ID_MIX_UY        = s.new()   # FMix result y\n'
    '    ID_MIX_TIDX      = s.new()   # FMix result texIdx\n'
)
NEW_IDS = (
    '    ID_MIX_UX        = s.new()   # FMix result x\n'
    '    ID_MIX_UY        = s.new()   # FMix result y\n'
    '    ID_MIX_TIDX      = s.new()   # FMix result texIdx\n'
    '    # UV quantization (PS1 low-precision UV – makes the effect clearly visible)\n'
    '    ID_CPREC         = s.new()   # constant 32.0 (UV grid steps)\n'
    '    ID_QUX           = s.new()   # MIX_UX * prec\n'
    '    ID_QUY           = s.new()   # MIX_UY * prec\n'
    '    ID_QUX5          = s.new()   # QUX + 0.5 (for rounding)\n'
    '    ID_QUY5          = s.new()   # QUY + 0.5\n'
    '    ID_FQUX          = s.new()   # floor(QUX5)\n'
    '    ID_FQUY          = s.new()   # floor(QUY5)\n'
    '    ID_SQUX          = s.new()   # FQUX / prec (snapped x)\n'
    '    ID_SQUY          = s.new()   # FQUY / prec (snapped y)\n'
    '    ID_FUX           = s.new()   # mix(MIX_UX, SQUX, AFFINE_MIX) – final U\n'
    '    ID_FUY           = s.new()   # mix(MIX_UY, SQUY, AFFINE_MIX) – final V\n'
)

if src.count(OLD_IDS) != 1:
    print(f'STEP1 FAILED: {src.count(OLD_IDS)} matches for ID block')
    sys.exit(1)
src = src.replace(OLD_IDS, NEW_IDS, 1)
print('STEP1 OK – added UV quantization IDs')

# ─────────────────────────────────────────────────────────────────────────────
# 2. Add the CPREC constant to the OP_CONSTANT section in build_fragment()
#    Anchor: ID_ALPHA_THRESH constant (unique per-function)
# ─────────────────────────────────────────────────────────────────────────────
OLD_CONST = (
    '    s.emit(OP_CONSTANT, ID_FLOAT, ID_ALPHA_THRESH, f2w(0.05))\n'
    '    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC3, ID_LIGHT_VEC, ID_LC, ID_LC, ID_LC)\n'
    '    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC3, ID_ONE_VEC,   ID_C1, ID_C1, ID_C1)\n'
)
NEW_CONST = (
    '    s.emit(OP_CONSTANT, ID_FLOAT, ID_ALPHA_THRESH, f2w(0.05))\n'
    '    s.emit(OP_CONSTANT, ID_FLOAT, ID_CPREC,        f2w(32.0))  # UV quantization precision\n'
    '    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC3, ID_LIGHT_VEC, ID_LC, ID_LC, ID_LC)\n'
    '    s.emit(OP_CONSTANT_COMPOSITE, ID_VEC3, ID_ONE_VEC,   ID_C1, ID_C1, ID_C1)\n'
)
if src.count(OLD_CONST) != 1:
    print(f'STEP2 FAILED: {src.count(OLD_CONST)} matches for ALPHA_THRESH block')
    sys.exit(1)
src = src.replace(OLD_CONST, NEW_CONST, 1)
print('STEP2 OK – added CPREC constant')

# ─────────────────────────────────────────────────────────────────────────────
# 3. Replace the final OP_COMPOSITE_CONSTRUCT that builds ID_UVW
#    (where the FMix results feed into the texture coordinate)
#    Add quantization between FMix and the vec3 construct.
# ─────────────────────────────────────────────────────────────────────────────
OLD_CONSTRUCT = (
    '    s.emit(OP_EXT_INST, ID_FLOAT, ID_MIX_TIDX, ID_GLSL_EXT, GLSL_FMIX, ID_LTIDX,     ID_LAFF_TIDX, ID_AFFINE_MIX)\n'
    '    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC3, ID_UVW, ID_MIX_UX, ID_MIX_UY, ID_MIX_TIDX)\n'
)
NEW_CONSTRUCT = (
    '    s.emit(OP_EXT_INST, ID_FLOAT, ID_MIX_TIDX, ID_GLSL_EXT, GLSL_FMIX, ID_LTIDX,     ID_LAFF_TIDX, ID_AFFINE_MIX)\n'
    '    # UV quantization: snap to 32-step grid to simulate PS1 low-precision UV\n'
    '    # floor(uv * 32 + 0.5) / 32 – then blend with affine_mix\n'
    '    GLSL_FLOOR = 8\n'
    '    s.emit(OP_F_MUL,    ID_FLOAT, ID_QUX,  ID_MIX_UX, ID_CPREC)\n'
    '    s.emit(OP_F_MUL,    ID_FLOAT, ID_QUY,  ID_MIX_UY, ID_CPREC)\n'
    '    s.emit(OP_F_ADD,    ID_FLOAT, ID_QUX5, ID_QUX, ID_C05)\n'
    '    s.emit(OP_F_ADD,    ID_FLOAT, ID_QUY5, ID_QUY, ID_C05)\n'
    '    s.emit(OP_EXT_INST, ID_FLOAT, ID_FQUX, ID_GLSL_EXT, GLSL_FLOOR, ID_QUX5)\n'
    '    s.emit(OP_EXT_INST, ID_FLOAT, ID_FQUY, ID_GLSL_EXT, GLSL_FLOOR, ID_QUY5)\n'
    '    s.emit(OP_F_DIV,    ID_FLOAT, ID_SQUX, ID_FQUX, ID_CPREC)\n'
    '    s.emit(OP_F_DIV,    ID_FLOAT, ID_SQUY, ID_FQUY, ID_CPREC)\n'
    '    s.emit(OP_EXT_INST, ID_FLOAT, ID_FUX,  ID_GLSL_EXT, GLSL_FMIX, ID_MIX_UX, ID_SQUX, ID_AFFINE_MIX)\n'
    '    s.emit(OP_EXT_INST, ID_FLOAT, ID_FUY,  ID_GLSL_EXT, GLSL_FMIX, ID_MIX_UY, ID_SQUY, ID_AFFINE_MIX)\n'
    '    s.emit(OP_COMPOSITE_CONSTRUCT, ID_VEC3, ID_UVW, ID_FUX, ID_FUY, ID_MIX_TIDX)\n'
)
if src.count(OLD_CONSTRUCT) != 1:
    print(f'STEP3 FAILED: {src.count(OLD_CONSTRUCT)} matches for CONSTRUCT block')
    sys.exit(1)
src = src.replace(OLD_CONSTRUCT, NEW_CONSTRUCT, 1)
print('STEP3 OK – added UV quantization ops before texture sample')

# ─────────────────────────────────────────────────────────────────────────────
# Write back
# ─────────────────────────────────────────────────────────────────────────────
with open('tools/gen_shaders.py', 'w', encoding='utf-8') as f:
    f.write(src)
print('gen_shaders.py written.')
