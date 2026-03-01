#!/usr/bin/env python3
"""
Extract all icon states from .dmi files in legacysets/icons into PNG/GIF files.
Non-animated states (1 frame) become PNGs; animated states (>1 frame) become GIFs.

Output mirrors the source directory structure under legacysets/extracted/.
"""

import os
import re
import sys
import math
import struct
import zlib
from pathlib import Path

from PIL import Image

# ──────────────────────────────────────────────────────────
# DMI metadata parser
# ──────────────────────────────────────────────────────────

def _read_png_text_chunks(path: str) -> dict[str, str]:
    """Return all tEXt/zTXt chunks from a PNG as {keyword: text}."""
    result: dict[str, str] = {}
    with open(path, "rb") as f:
        sig = f.read(8)
        if sig != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"Not a PNG file: {path}")
        while True:
            header = f.read(8)
            if len(header) < 8:
                break
            length = struct.unpack(">I", header[:4])[0]
            chunk_type = header[4:8].decode("ascii", errors="replace")
            data = f.read(length)
            f.read(4)  # CRC
            if chunk_type == "tEXt":
                null = data.index(b"\x00")
                keyword = data[:null].decode("latin-1")
                text = data[null + 1:].decode("latin-1")
                result[keyword] = text
            elif chunk_type == "zTXt":
                null = data.index(b"\x00")
                keyword = data[:null].decode("latin-1")
                # skip compression method byte
                text = zlib.decompress(data[null + 2:]).decode("latin-1")
                result[keyword] = text
            elif chunk_type == "IEND":
                break
    return result


def parse_dmi_metadata(desc: str) -> tuple[int, int, list[dict]]:
    """
    Parse the DMI Description string.
    Returns (icon_width, icon_height, states) where each state is a dict:
      { name, dirs, frames, delay, loop, rewind, movement, hotspot }
    """
    lines = desc.splitlines()
    icon_w = icon_h = 32
    states: list[dict] = []
    current: dict | None = None

    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip()

        if key == "width":
            icon_w = int(val)
        elif key == "height":
            icon_h = int(val)
        elif key == "state":
            if current is not None:
                states.append(current)
            name = val.strip('"')
            current = {
                "name": name,
                "dirs": 1,
                "frames": 1,
                "delay": [],
                "loop": 0,
                "rewind": 0,
                "movement": 0,
            }
        elif current is not None:
            if key == "dirs":
                current["dirs"] = int(val)
            elif key == "frames":
                current["frames"] = int(val)
            elif key == "delay":
                current["delay"] = [float(x) for x in val.split(",") if x.strip()]
            elif key == "loop":
                current["loop"] = int(val)
            elif key == "rewind":
                current["rewind"] = int(val)
            elif key == "movement":
                current["movement"] = int(val)

    if current is not None:
        states.append(current)

    return icon_w, icon_h, states


# ──────────────────────────────────────────────────────────
# Sprite extraction
# ──────────────────────────────────────────────────────────

DIR_SUFFIX = {1: [""], 4: ["_s", "_n", "_e", "_w"], 8: ["_s", "_n", "_e", "_w", "_se", "_sw", "_ne", "_nw"]}

def safe_filename(name: str) -> str:
    """Replace characters that are illegal in Windows filenames."""
    return re.sub(r'[\\/:*?"<>|]', "_", name) if name else "_blank"


def extract_dmi(dmi_path: Path, out_dir: Path) -> int:
    """
    Extract all states from one DMI file into out_dir.
    Returns the number of files written.
    """
    try:
        chunks = _read_png_text_chunks(str(dmi_path))
    except Exception as e:
        print(f"  [WARN] Cannot read {dmi_path.name}: {e}", flush=True)
        return 0

    desc = chunks.get("Description", "")
    if not desc:
        print(f"  [WARN] No DMI Description in {dmi_path.name}", flush=True)
        return 0

    try:
        icon_w, icon_h, states = parse_dmi_metadata(desc)
    except Exception as e:
        print(f"  [WARN] Metadata parse error in {dmi_path.name}: {e}", flush=True)
        return 0

    try:
        sheet = Image.open(str(dmi_path)).convert("RGBA")
    except Exception as e:
        print(f"  [WARN] Cannot open image {dmi_path.name}: {e}", flush=True)
        return 0

    sheet_w, sheet_h = sheet.size
    cols = sheet_w // icon_w if icon_w else 1

    out_dir.mkdir(parents=True, exist_ok=True)

    written = 0
    linear_index = 0  # which cell in the sheet we're at

    # Track duplicate state names within the same DMI
    name_counts: dict[str, int] = {}

    for state in states:
        raw_name = state["name"]
        dirs = state["dirs"]
        frames = state["frames"]
        delay = state["delay"]
        loop = state["loop"]  # 0 = loop forever in BYOND terms

        total_images = dirs * frames

        # Collect all cell images for this state
        cells: list[Image.Image] = []
        for _ in range(total_images):
            col = linear_index % cols
            row = linear_index // cols
            x = col * icon_w
            y = row * icon_h
            cell = sheet.crop((x, y, x + icon_w, y + icon_h))
            cells.append(cell)
            linear_index += 1

        # Build per-direction frame lists
        # BYOND layout: dir0-frame0, dir0-frame1, ..., dir1-frame0, ...
        dir_frames: list[list[Image.Image]] = []
        for d in range(dirs):
            dir_frames.append([cells[d * frames + f] for f in range(frames)])

        dir_suffixes = DIR_SUFFIX.get(dirs, [""] * dirs)

        # Deduplicate state name
        base_name = safe_filename(raw_name)
        if base_name in name_counts:
            name_counts[base_name] += 1
            base_name = f"{base_name}_{name_counts[base_name]}"
        else:
            name_counts[base_name] = 0

        for d, (dir_suffix, frame_list) in enumerate(zip(dir_suffixes, dir_frames)):
            fname_stem = f"{base_name}{dir_suffix}"

            if frames == 1:
                # Static → PNG
                out_path = out_dir / f"{fname_stem}.png"
                frame_list[0].save(str(out_path), "PNG")
                written += 1
            else:
                # Animated → GIF
                out_path = out_dir / f"{fname_stem}.gif"
                # Build frame durations (BYOND delay unit = 1 tick = 100ms)
                # BYOND delay is in ticks (1 tick = 100 ms).
                # GIF stores centiseconds (0-65535). Pillow takes ms and divides by 10,
                # so cap at 655350 ms (= 65535 cs). Minimum 10 ms (1 cs).
                if delay and len(delay) >= frames:
                    durations = [min(655350, max(10, int(delay[i] * 100))) for i in range(frames)]
                else:
                    durations = [100] * frames  # default 1 tick each

                # Build rewind frames if requested
                all_frames = frame_list[:]
                all_durations = durations[:]
                if state.get("rewind"):
                    all_frames = frame_list + list(reversed(frame_list[1:-1]))
                    all_durations = durations + list(reversed(durations[1:-1]))

                # GIF loop count: BYOND 0 = infinite, n = n loops → PIL loop=0 means infinite
                gif_loop = 0 if loop == 0 else loop

                # Convert frames to palette mode for GIF
                gif_frames = [f.convert("RGBA") for f in all_frames]
                try:
                    gif_frames[0].save(
                        str(out_path),
                        "GIF",
                        save_all=True,
                        append_images=gif_frames[1:],
                        duration=all_durations,
                        loop=gif_loop,
                        disposal=2,
                    )
                    written += 1
                except Exception as gif_err:
                    print(f"  [WARN] GIF save failed for {out_path.name}: {gif_err}", flush=True)

    return written


# ──────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────

def main() -> None:
    workspace = Path(__file__).resolve().parent.parent
    src_root = workspace / "legacysets" / "icons"
    dst_root = workspace / "legacysets" / "extracted"

    if not src_root.exists():
        print(f"Source folder not found: {src_root}", file=sys.stderr)
        sys.exit(1)

    dmi_files = sorted(src_root.rglob("*.dmi"))
    total_dmis = len(dmi_files)
    print(f"Found {total_dmis} DMI files under {src_root}", flush=True)

    total_written = 0
    for i, dmi_path in enumerate(dmi_files, 1):
        rel = dmi_path.relative_to(src_root)
        # Output goes to: legacysets/extracted/<rel_parent>/<dmi_stem>/
        out_dir = dst_root / rel.parent / rel.stem
        # Skip only if a marker file confirms this exact DMI was already extracted.
        # (A same-named subdirectory populated by child DMIs must not be skipped.)
        marker = out_dir / ".dmi_done"
        if marker.exists():
            total_written += sum(1 for _ in out_dir.rglob("*") if _.is_file() and _.name != ".dmi_done")
            continue
        print(f"[{i}/{total_dmis}] {rel} -> {out_dir.relative_to(workspace)}", flush=True)
        n = extract_dmi(dmi_path, out_dir)
        # Write marker so subsequent runs know this DMI is done.
        marker.write_text(dmi_path.name)
        print(f"  wrote {n} file(s)", flush=True)
        total_written += n

    print(f"\nDone. {total_written} files extracted to {dst_root}", flush=True)


if __name__ == "__main__":
    main()
