#!/usr/bin/env python3
"""fbx_to_mesh.py – Convert a binary FBX mesh to a simple custom .mesh format.

Output .mesh format (little-endian):
  4 bytes  magic "MESH"
  4 bytes  uint32 num_vertices
  4 bytes  uint32 num_indices
  N * 36 bytes  vertices: (x,y,z, nx,ny,nz, u,v, tex_idx) each float32
  M * 4 bytes   indices: uint32

Usage:
  python tools/fbx_to_mesh.py models/SMES.fbx models/SMES.mesh
"""

import struct, sys, zlib, math, pathlib, argparse

# ─── FBX Binary Parser ────────────────────────────────────────────────────────

FBX_MAGIC = b"Kaydara FBX Binary  \x00\x1a\x00"

def _read_props(data: bytes, offset: int, count: int):
    """Read 'count' property values from data starting at offset.
    Returns list of values and new offset."""
    props = []
    for _ in range(count):
        type_code = chr(data[offset]); offset += 1
        if type_code == 'Y':
            v, = struct.unpack_from('<h', data, offset); offset += 2; props.append(v)
        elif type_code == 'C':
            v = data[offset]; offset += 1; props.append(bool(v & 1))
        elif type_code == 'I':
            v, = struct.unpack_from('<i', data, offset); offset += 4; props.append(v)
        elif type_code == 'F':
            v, = struct.unpack_from('<f', data, offset); offset += 4; props.append(v)
        elif type_code == 'D':
            v, = struct.unpack_from('<d', data, offset); offset += 8; props.append(v)
        elif type_code == 'L':
            v, = struct.unpack_from('<q', data, offset); offset += 8; props.append(v)
        elif type_code in ('S', 'R'):
            length, = struct.unpack_from('<I', data, offset); offset += 4
            raw = data[offset:offset+length]; offset += length
            props.append(raw.decode('utf-8', errors='replace') if type_code == 'S' else raw)
        elif type_code in ('b', 'i', 'l', 'f', 'd'):
            arr_len,   = struct.unpack_from('<I', data, offset); offset += 4
            encoding,  = struct.unpack_from('<I', data, offset); offset += 4
            comp_len,  = struct.unpack_from('<I', data, offset); offset += 4
            raw = data[offset:offset+comp_len]; offset += comp_len
            if encoding == 1:
                raw = zlib.decompress(raw)
            fmt_map = {'b': 'B', 'i': 'i', 'l': 'q', 'f': 'f', 'd': 'd'}
            fmt = '<' + fmt_map[type_code] * arr_len
            arr = list(struct.unpack_from(fmt, raw))
            props.append(arr)
        else:
            raise ValueError(f"Unknown FBX property type '{type_code}' at offset {offset-1}")
    return props, offset


def _parse_node(data: bytes, offset: int, version: int):
    """Parse one FBX node.  Returns (name, props, children, end_offset) or None at null record."""
    big = version >= 7500

    if big:
        if offset + 25 > len(data): return None
        end_off,    = struct.unpack_from('<Q', data, offset); offset += 8
        num_props,  = struct.unpack_from('<Q', data, offset); offset += 8
        _prop_len,  = struct.unpack_from('<Q', data, offset); offset += 8  # unused
    else:
        if offset + 13 > len(data): return None
        end_off,    = struct.unpack_from('<I', data, offset); offset += 4
        num_props,  = struct.unpack_from('<I', data, offset); offset += 4
        _prop_len,  = struct.unpack_from('<I', data, offset); offset += 4

    name_len = data[offset]; offset += 1
    name = data[offset:offset+name_len].decode('utf-8', errors='replace'); offset += name_len

    if end_off == 0:
        return None  # null sentinel record

    props, offset = _read_props(data, offset, num_props)

    children = []
    null_block = 25 if big else 13
    while offset < end_off - null_block:
        child = _parse_node(data, offset, version)
        if child is None:
            break
        _name, _props, _children, child_end = child
        children.append({'name': _name, 'props': _props, 'children': _children})
        offset = child_end

    return name, props, children, end_off


def parse_fbx(path: str):
    """Parse a binary FBX file.  Returns list of top-level node dicts."""
    data = pathlib.Path(path).read_bytes()
    if not data.startswith(FBX_MAGIC):
        raise ValueError("Not a binary FBX file")
    version, = struct.unpack_from('<I', data, 23)
    print(f"FBX version: {version}")

    offset = 27  # magic(23) + version(4)
    nodes = []
    big = version >= 7500
    null_block = 25 if big else 13
    while offset < len(data) - null_block:
        result = _parse_node(data, offset, version)
        if result is None:
            break
        name, props, children, end_off = result
        nodes.append({'name': name, 'props': props, 'children': children})
        offset = end_off
    return nodes


def find_all(nodes, name: str):
    """Recursively find all nodes with given name."""
    results = []
    for n in nodes:
        if n['name'] == name:
            results.append(n)
        results.extend(find_all(n['children'], name))
    return results


def first_prop_array(node, child_name: str):
    """Find first child node with given name and return its first array property."""
    for c in node['children']:
        if c['name'] == child_name:
            for p in c['props']:
                if isinstance(p, list):
                    return p
    return None


# ─── Mesh Extraction ──────────────────────────────────────────────────────────

def extract_mesh(geo_node):
    """Extract triangulated vertex data from a Geometry node.
    Returns list of (x,y,z, nx,ny,nz, u,v) tuples (float32-ready) and index list."""

    raw_verts = first_prop_array(geo_node, 'Vertices')     # float64 x,y,z,...
    poly_idx  = first_prop_array(geo_node, 'PolygonVertexIndex')  # int32

    if not raw_verts or not poly_idx:
        return None, None

    # Package positions: raw_verts = [x0,y0,z0, x1,y1,z1, ...]
    positions = [(raw_verts[i], raw_verts[i+1], raw_verts[i+2])
                 for i in range(0, len(raw_verts), 3)]

    # Find normals (LayerElementNormal)
    normals_per_pv = []
    for child in geo_node['children']:
        if child['name'] == 'LayerElementNormal':
            ref_type = ''
            map_type = ''
            for cc in child['children']:
                if cc['name'] == 'ReferenceInformationType':
                    ref_type = cc['props'][0] if cc['props'] else ''
                if cc['name'] == 'MappingInformationType':
                    map_type = cc['props'][0] if cc['props'] else ''
            raw_n = first_prop_array(child, 'Normals') or []
            n_idx = first_prop_array(child, 'NormalsIndex') or []
            if raw_n:
                if ref_type == 'IndexToDirect':
                    normals_per_pv = [(raw_n[n_idx[i]*3],   raw_n[n_idx[i]*3+1],
                                       raw_n[n_idx[i]*3+2]) for i in range(len(n_idx))]
                else:  # Direct
                    normals_per_pv = [(raw_n[i], raw_n[i+1], raw_n[i+2])
                                      for i in range(0, len(raw_n), 3)]
            break

    # Find UVs (LayerElementUV)
    uvs_per_pv = []
    for child in geo_node['children']:
        if child['name'] == 'LayerElementUV':
            ref_type = ''
            for cc in child['children']:
                if cc['name'] == 'ReferenceInformationType':
                    ref_type = cc['props'][0] if cc['props'] else ''
            raw_uv  = first_prop_array(child, 'UV') or []
            uv_idx  = first_prop_array(child, 'UVIndex') or []
            if raw_uv:
                if ref_type == 'IndexToDirect' and uv_idx:
                    uvs_per_pv = [(raw_uv[uv_idx[i]*2], raw_uv[uv_idx[i]*2+1])
                                  for i in range(len(uv_idx))]
                else:
                    uvs_per_pv = [(raw_uv[i], raw_uv[i+1])
                                  for i in range(0, len(raw_uv), 2)]
            break

    # Build face list from PolygonVertexIndex
    # Negative index marks end of polygon (actual index = ~value)
    faces = []
    current = []
    pv_cursor = 0  # index into per-polygon-vertex arrays
    pv_face_starts = []  # pv cursor at start of each face

    for iv in poly_idx:
        if iv < 0:
            current.append(~iv)
            pv_face_starts.append(pv_cursor - len(current) + 1)
            faces.append((list(current), pv_cursor - len(current) + 1))
            current = []
        else:
            current.append(iv)
        pv_cursor += 1

    # Triangulate (fan) and build final vertex + index buffers
    out_verts  = []
    out_idx    = []
    vert_cache = {}  # (vi, ni_f, ui_f) → output_index

    def get_norm(pv_i):
        if pv_i < len(normals_per_pv):
            return normals_per_pv[pv_i]
        return (0.0, 1.0, 0.0)  # fallback up-normal

    def get_uv(pv_i):
        if pv_i < len(uvs_per_pv):
            return uvs_per_pv[pv_i]
        return (0.0, 0.0)

    def add_vert(vi, pv_i):
        px, py, pz = positions[vi]
        nx, ny, nz = get_norm(pv_i)
        u,  v      = get_uv(pv_i)
        # Round to reduce duplicates
        key = (vi, round(nx,4), round(ny,4), round(nz,4), round(u,4), round(v,4))
        if key not in vert_cache:
            vert_cache[key] = len(out_verts)
            out_verts.append((float(px), float(py), float(pz),
                              float(nx), float(ny), float(nz),
                              float(u),  float(v)))
        return vert_cache[key]

    for vis, pv_start in faces:
        if len(vis) < 3:
            continue
        # Fan triangulation from vis[0]
        for k in range(1, len(vis) - 1):
            i0 = add_vert(vis[0], pv_start)
            i1 = add_vert(vis[k],   pv_start + k)
            i2 = add_vert(vis[k+1], pv_start + k + 1)
            out_idx.extend([i0, i1, i2])

    return out_verts, out_idx


# ─── PNG generation ───────────────────────────────────────────────────────────

def _adler32(data: bytes) -> int:
    s1, s2 = 1, 0
    for b in data:
        s1 = (s1 + b)   % 65521
        s2 = (s2 + s1)  % 65521
    return (s2 << 16) | s1

def write_png(path: str, width: int, height: int, rgba: bytes):
    """Write a minimal PNG file (RGBA, 8-bit)."""
    import zlib, struct
    def chunk(tag: bytes, data: bytes) -> bytes:
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    # IHDR
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    # Raw image data (filter byte 0 per scanline)
    raw = b''
    for y in range(height):
        raw += b'\x00' + rgba[y*width*4:(y+1)*width*4]
    idat = zlib.compress(raw, 9)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', idat))
        f.write(chunk(b'IEND', b''))


def make_smes_texture(path: str, w: int = 64, h: int = 64):
    """Generate a simple steel-blue SMES placeholder texture."""
    pixels = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            # Border / panel lines
            on_grid = (x % 16 < 1) or (y % 16 < 1)
            # Charge indicator stripe in the middle
            in_stripe = (h // 3 <= y < 2 * h // 3) and (w//4 <= x < 3*w//4)
            if on_grid:
                r, g, b = 30,  35,  50
            elif in_stripe:
                r, g, b = 40, 120, 200  # blue charge bar
            else:
                r, g, b = 60,  70,  90  # dark steel
            idx = (y * w + x) * 4
            pixels[idx+0] = r
            pixels[idx+1] = g
            pixels[idx+2] = b
            pixels[idx+3] = 255
    write_png(path, w, h, bytes(pixels))
    print(f"Wrote texture: {path}")


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Convert binary FBX to .mesh")
    parser.add_argument('input',  help="Input .fbx file")
    parser.add_argument('output', help="Output .mesh file")
    parser.add_argument('--texture', default=None,
                        help="Generate a placeholder texture at this path")
    args = parser.parse_args()

    print(f"Parsing {args.input}...")
    nodes = parse_fbx(args.input)

    # Find all Geometry nodes
    geo_nodes = find_all(nodes, 'Geometry')
    mesh_geos = [g for g in geo_nodes
                 if any(p == 'Mesh' for p in g['props'] if isinstance(p, str))]

    if not mesh_geos:
        print("ERROR: No Geometry[Mesh] nodes found in FBX")
        sys.exit(1)

    print(f"Found {len(mesh_geos)} mesh geometry node(s). Using first.")
    geo = mesh_geos[0]

    verts, indices = extract_mesh(geo)
    if verts is None:
        print("ERROR: Failed to extract mesh from Geometry node")
        sys.exit(1)

    print(f"Extracted {len(verts)} vertices, {len(indices)} indices ({len(indices)//3} triangles)")

    # Write .mesh
    out = pathlib.Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, 'wb') as f:
        f.write(b'MESH')
        f.write(struct.pack('<II', len(verts), len(indices)))
        for (px, py, pz, nx, ny, nz, u, v) in verts:
            f.write(struct.pack('<fffffffff', px, py, pz, nx, ny, nz, u, v, 0.0))
        for i in indices:
            f.write(struct.pack('<I', i))
    print(f"Wrote mesh: {out}  ({out.stat().st_size} bytes)")

    if args.texture:
        tex_path = pathlib.Path(args.texture)
        tex_path.parent.mkdir(parents=True, exist_ok=True)
        make_smes_texture(str(tex_path))


if __name__ == '__main__':
    main()
