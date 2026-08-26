#!/usr/bin/env python3
"""Decode Katana's fixed native graphics breadcrumb ring to JSONL."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path


HEADER = struct.Struct("<8sIIIIQQQQQ")
RECORD = struct.Struct("<17Q32s32s11I10B2x")
MAGIC = b"KATGFXB2"

MODES = {0: "off", 1: "digest", 2: "breadcrumbs", 3: "armed-capture"}
ORIGINS = {
    0: "unspecified",
    1: "immediate",
    2: "model-mesh",
    3: "sprite",
    4: "font",
    5: "movie",
    6: "presentation",
}
INTENTS = {
    0: "unspecified",
    1: "scene-object",
    2: "shadow",
    3: "interface",
    4: "font",
    5: "movie",
    6: "presentation",
}
RESOLVERS = {
    0: "unspecified",
    1: "native-descriptor",
    2: "dynamic-surface",
    3: "cached-archive",
    4: "exact-archive-names",
    5: "exact-archive-layouts",
    6: "partial-archive",
    7: "checkpoint",
    8: "registered-texture",
    9: "identity-bound-override",
}
WRITERS = {
    0: "unspecified",
    1: "texture-list-bind",
    2: "texture-number-select",
    3: "registered-texture-select",
    4: "identity-bound-override",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def emit(output, value: dict) -> None:
    output.write(json.dumps(value, separators=(",", ":"), sort_keys=True))
    output.write("\n")


def main() -> int:
    args = parse_args()
    data = args.input.read_bytes()
    if len(data) < HEADER.size:
        raise SystemExit("breadcrumb file is shorter than its header")
    header = HEADER.unpack_from(data)
    magic, schema, header_size, record_size, mode, capacity, count, first, drops, digest = header
    if magic != MAGIC or schema != 2:
        raise SystemExit("unsupported breadcrumb magic or schema")
    if header_size != HEADER.size or record_size != RECORD.size:
        raise SystemExit("breadcrumb ABI size does not match decoder")
    if count > capacity:
        raise SystemExit("breadcrumb count exceeds capacity")
    expected_size = header_size + count * record_size
    if len(data) != expected_size:
        raise SystemExit("breadcrumb file length does not match header")

    output = args.output.open("w", encoding="utf-8", newline="\n") if args.output else sys.stdout
    try:
        emit(
            output,
            {
                "schema": "katana-native-graphics-breadcrumbs-v2",
                "record": "header",
                "mode": MODES.get(mode, mode),
                "capacity": capacity,
                "count": count,
                "ring_first_record": first,
                "dropped": drops,
                "final_digest": digest,
            },
        )
        offset = header_size
        for ordinal in range(count):
            values = RECORD.unpack_from(data, offset)
            offset += record_size
            words = values[:17]
            content_sha = values[17]
            decoded_payload_sha = values[18]
            integers = values[19:30]
            enums = values[30:40]
            (
                frame,
                draw,
                batch_identity,
                global_digest,
                frame_digest,
                layer_digest,
                material_identity,
                origin_identity,
                model_identity,
                texture_handle,
                texture_list_identity,
                texture_list_epoch,
                last_writer_identity,
                last_writer_sequence,
                expected_asset_identity,
                resolved_asset_identity,
                texture_generation,
            ) = words
            (
                archive_ordinal,
                global_index,
                texture_list_index,
                texture_list_count,
                mesh_index,
                primitive_index,
                submission_order,
                flags,
                texture_width,
                texture_height,
                texture_mip_levels,
            ) = integers
            (
                texture_source_pixel_format,
                texture_source_data_format,
                batch_semantic,
                draw_class,
                logical_use,
                origin,
                intent,
                resolver,
                writer,
                _,
            ) = enums
            record = {
                "schema": "katana-native-graphics-breadcrumb-v2",
                "record": ordinal,
                "frame": frame,
                "draw": draw,
                "batch_identity": batch_identity,
                "batch_semantic": batch_semantic,
                "draw_class": draw_class,
                "logical_use": logical_use,
                "submission_order": submission_order,
                "digests": {
                    "global": global_digest,
                    "frame": frame_digest,
                    "layer": layer_digest,
                },
                "origin": {
                    "kind": ORIGINS.get(origin, origin),
                    "identity": origin_identity if flags & (1 << 3) else None,
                    "intent": INTENTS.get(intent, intent),
                    "model_identity": model_identity if flags & (1 << 4) else None,
                    "mesh_index": mesh_index,
                    "primitive_index": primitive_index,
                },
                "material_identity": material_identity if flags & (1 << 2) else None,
                "texture": {
                    "handle": texture_handle,
                    "list_identity": texture_list_identity if flags & (1 << 6) else None,
                    "list_epoch": texture_list_epoch if flags & (1 << 12) else None,
                    "list_index": texture_list_index if flags & (1 << 5) else None,
                    "list_count": texture_list_count if flags & (1 << 6) else None,
                    "resolver": RESOLVERS.get(resolver, resolver),
                    "last_writer": WRITERS.get(writer, writer),
                    "last_writer_identity": last_writer_identity if flags & (1 << 7) else None,
                    "last_writer_sequence": last_writer_sequence if flags & (1 << 7) else None,
                    "expected_asset_identity": expected_asset_identity if flags & (1 << 8) else None,
                    "resolved_asset_identity": resolved_asset_identity if flags & (1 << 9) else None,
                    "resource_generation": texture_generation,
                    "archive_ordinal": archive_ordinal,
                    "global_index": global_index if flags & (1 << 11) else None,
                    "content_sha256": content_sha.hex() if flags & (1 << 10) else None,
                    "decoded_rgba8_sha256": (
                        decoded_payload_sha.hex() if flags & (1 << 14) else None
                    ),
                    "source_pixel_format": (
                        texture_source_pixel_format if flags & (1 << 14) else None
                    ),
                    "source_data_format": (
                        texture_source_data_format if flags & (1 << 14) else None
                    ),
                    "width": texture_width if flags & (1 << 14) else None,
                    "height": texture_height if flags & (1 << 14) else None,
                    "mip_levels": (
                        texture_mip_levels if flags & (1 << 14) else None
                    ),
                },
                "geometry_available": bool(flags & 1),
                "rejected": bool(flags & 2),
                "adapter_diagnostics_enabled": bool(flags & (1 << 13)),
            }
            emit(output, record)
    finally:
        if output is not sys.stdout:
            output.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
