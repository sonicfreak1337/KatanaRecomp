#!/usr/bin/env python3
"""Write a public, deterministic NativeDisc cold-build stress fixture.

The generated disc contains only bytes produced by this file.  It deliberately
models a large analysis/cache/partition workload without depending on retail
game data or on a local Katana build.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


LOGICAL_SECTOR_SIZE = 2_048
RAW_SECTOR_SIZE = 2_352
DATA_LBA = 45_000
BOOT_LOAD_ADDRESS = 0x8C01_0000
SEMANTIC_LOAD_ADDRESS = 0x8D00_0000
BLOCKS_PER_FUNCTION = 14
FUNCTION_SIZE = BLOCKS_PER_FUNCTION * 4
SEMANTIC_HEAVY_BLOCKS = 4_096
SEMANTIC_ROOT_TAIL = SEMANTIC_HEAVY_BLOCKS * 4
SEMANTIC_SIZE = 0x5148
DETERMINISTIC_SEED = 0x00C0FFEE
ROOT_MAGIC = b"KSTRROOT"
ROOT_HEADER = struct.Struct("<8sIIII")
ROOT_RECORD = struct.Struct("<IIIII")
ROOT_SCHEMA_VERSION = 2
MANIFEST_SCHEMA = "katana-native-disc-cold-build-stress-v3"
MANIFEST_SCHEMA_VERSION = 3
WORKLOAD_SCHEMA = "katana-native-disc-workload-v3"
WORKLOAD_SCHEMA_VERSION = 3
PROVENANCE_CONTRACT_VERSION = 3
DELTA_SCALE_FUNCTION_COUNTS = [16, 128]
REQUIRED_FUNCTION_VALUE_SUBPHASES = [
    "inventory-region-closure",
    "inventory-region-sink-sources",
    "abi-return-signatures",
    "abi-stack-reads",
    "abi-register-reads",
    "persistent-store-signatures",
    "inventory-reachability",
    "cache-key-plan",
    "resolution-root-dependencies",
    "resolution-root-scc-order",
    "resolution-root-scc-components",
    "resolution-root-contracts",
    "resolution-root-plan",
]
RESOLUTION_PROBE_VALUE_BLOCK = 0
RESOLUTION_PROBE_STORE_BLOCK = 1
RESOLUTION_PROBE_LOAD_BLOCK = 2


@dataclass(frozen=True)
class Profile:
    name: str
    function_count: int
    root_count: int
    seed_wave_counts: tuple[int, ...]
    partition_count: int
    module_count: int
    chunks_per_module: int
    chunk_size: int
    replay_passes: int


PROFILES = {
    "smoke": Profile(
        name="smoke",
        function_count=16,
        root_count=14,
        seed_wave_counts=(8, 3, 2, 1),
        partition_count=4,
        module_count=2,
        chunks_per_module=2,
        chunk_size=256,
        replay_passes=1,
    ),
    "reference": Profile(
        name="reference",
        function_count=1_600,
        root_count=1_400,
        seed_wave_counts=(800, 240, 120, 80, 60, 40, 30, 30),
        partition_count=64,
        module_count=32,
        chunks_per_module=8,
        chunk_size=512,
        replay_passes=53,
    ),
}

EXPECTED_MANIFEST_DIGESTS = {
    "smoke": "861af21607d2cdb1a1a8ea2a2ca5d85b0bab5995d58f4e62a49515bf94e818c6",
    "reference": "dbe0500286adc3e05de122a99769707e83e95302f4376ee608631864ac12af55",
}


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode(
        "utf-8"
    )


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_file(path: Path, data: bytes) -> None:
    with path.open("xb") as output:
        written = output.write(data)
        if written != len(data):
            raise OSError(f"short write for {path.name}")


def both_endian_u16(target: bytearray, offset: int, value: int) -> None:
    target[offset : offset + 2] = value.to_bytes(2, "little")
    target[offset + 2 : offset + 4] = value.to_bytes(2, "big")


def both_endian_u32(target: bytearray, offset: int, value: int) -> None:
    target[offset : offset + 4] = value.to_bytes(4, "little")
    target[offset + 4 : offset + 8] = value.to_bytes(4, "big")


def directory_record(extent_lba: int, byte_size: int, name: bytes, directory: bool) -> bytes:
    record_size = 33 + len(name) + (1 if len(name) % 2 == 0 else 0)
    record = bytearray(record_size)
    record[0] = record_size
    both_endian_u32(record, 2, extent_lba)
    both_endian_u32(record, 10, byte_size)
    record[18:25] = bytes((100, 1, 1, 0, 0, 0, 0))
    record[25] = 2 if directory else 0
    record[26] = 0
    record[27] = 0
    both_endian_u16(record, 28, 1)
    record[32] = len(name)
    record[33 : 33 + len(name)] = name
    return bytes(record)


def packed_directory_size(record_sizes: Iterable[int]) -> int:
    offset = 0
    for record_size in record_sizes:
        remaining = LOGICAL_SECTOR_SIZE - (offset % LOGICAL_SECTOR_SIZE)
        if record_size > remaining:
            offset += remaining
        offset += record_size
    return ((offset + LOGICAL_SECTOR_SIZE - 1) // LOGICAL_SECTOR_SIZE) * LOGICAL_SECTOR_SIZE


def pack_directory(records: Iterable[bytes], byte_size: int) -> bytes:
    output = bytearray(byte_size)
    offset = 0
    for record in records:
        remaining = LOGICAL_SECTOR_SIZE - (offset % LOGICAL_SECTOR_SIZE)
        if len(record) > remaining:
            offset += remaining
        output[offset : offset + len(record)] = record
        offset += len(record)
    if offset > byte_size:
        raise AssertionError("directory layout exceeded its reservation")
    return bytes(output)


def sh4_move_immediate(function_index: int, block_index: int) -> int:
    register = (function_index + block_index) & 0x0F
    immediate = (function_index * 29 + block_index * 17 + DETERMINISTIC_SEED) & 0xFF
    return 0xE000 | (register << 8) | immediate


def resolution_root_function_indices(profile: Profile) -> set[int]:
    roots = {
        (root_index * 37) % profile.function_count
        for root_index in range(profile.root_count)
    }
    if len(roots) != profile.root_count:
        raise AssertionError("resolution-root fixture contains duplicate functions")
    return roots


def resolution_probe_delay(function_index: int, block_index: int) -> int | None:
    if block_index == RESOLUTION_PROBE_VALUE_BLOCK:
        immediate = (
            function_index * 29 + block_index * 17 + DETERMINISTIC_SEED
        ) & 0xFF
        return 0xE000 | immediate  # mov #imm,r0
    if block_index == RESOLUTION_PROBE_STORE_BLOCK:
        return 0x2F06  # mov.l r0,@-r15
    if block_index == RESOLUTION_PROBE_LOAD_BLOCK:
        return 0x60F6  # mov.l @r15+,r0
    return None


def build_boot_program(profile: Profile) -> bytes:
    output = bytearray(profile.function_count * FUNCTION_SIZE)
    resolution_roots = resolution_root_function_indices(profile)
    cursor = 0
    for function_index in range(profile.function_count):
        for block_index in range(BLOCKS_PER_FUNCTION - 1):
            delay = sh4_move_immediate(function_index, block_index)
            if function_index in resolution_roots:
                probe_delay = resolution_probe_delay(function_index, block_index)
                if probe_delay is not None:
                    delay = probe_delay
            struct.pack_into("<HH", output, cursor, 0xA000, delay)
            cursor += 4
        struct.pack_into("<HH", output, cursor, 0x000B, 0x0009)
        cursor += 4
    return bytes(output)


def build_semantic_program() -> bytes:
    """Build the small dependency fixture; the large boot graph is throughput-only."""
    output = bytearray((0x09, 0x00) * (SEMANTIC_SIZE // 2))

    def put_u16(offset: int, value: int) -> None:
        struct.pack_into("<H", output, offset, value)

    def put_u32(offset: int, value: int) -> None:
        struct.pack_into("<I", output, offset, value)

    # A deliberately heavier canonical-prefix owner. Later tiny owners can
    # become ready while this real function is still being evaluated.
    for block_index in range(SEMANTIC_HEAVY_BLOCKS):
        offset = block_index * 4
        put_u16(offset, 0xA000)  # bra next block
        put_u16(offset + 2, sh4_move_immediate(0, block_index))
    tail = SEMANTIC_ROOT_TAIL
    put_u16(tail + 0x00, 0xD007)  # callback literal -> r0
    put_u16(tail + 0x02, 0x2F02)  # fifth ABI argument
    put_u16(tail + 0x04, 0xD107)  # A literal -> r1
    put_u16(tail + 0x06, 0x410B)  # indirect jsr @r1
    put_u16(tail + 0x08, 0x0009)
    put_u16(tail + 0x0A, 0x000B)
    put_u16(tail + 0x0C, 0x0009)
    put_u32(tail + 0x20, SEMANTIC_LOAD_ADDRESS + 0x50C0)
    put_u32(tail + 0x24, SEMANTIC_LOAD_ADDRESS + 0x5000)

    # A: consume the stack argument, persist it, then call B indirectly.
    put_u16(0x5000, 0x64F2)
    put_u16(0x5002, 0xD503)
    put_u16(0x5004, 0x2542)
    put_u16(0x5006, 0xD107)
    put_u16(0x5008, 0x410B)
    put_u16(0x500A, 0x0009)
    put_u16(0x500C, 0x000B)
    put_u16(0x500E, 0x0009)
    put_u32(0x5010, SEMANTIC_LOAD_ADDRESS + 0x5140)
    put_u32(0x5024, SEMANTIC_LOAD_ADDRESS + 0x5040)

    # B calls C and then A, creating a real A<->B summary SCC.
    put_u16(0x5040, 0xD107)
    put_u16(0x5042, 0x410B)
    put_u16(0x5044, 0x0009)
    put_u16(0x5046, 0xD108)
    put_u16(0x5048, 0x410B)
    put_u16(0x504A, 0x0009)
    put_u16(0x504C, 0x000B)
    put_u16(0x504E, 0x0009)
    put_u32(0x5060, SEMANTIC_LOAD_ADDRESS + 0x5080)
    put_u32(0x5068, SEMANTIC_LOAD_ADDRESS + 0x5000)

    # C extends the chain with a persistent store and indirect callback call.
    put_u16(0x5080, 0xD603)
    put_u16(0x5082, 0x2642)
    put_u16(0x5084, 0xD107)
    put_u16(0x5086, 0x410B)
    put_u16(0x5088, 0x0009)
    put_u16(0x508A, 0x000B)
    put_u16(0x508C, 0x0009)
    put_u32(0x5090, SEMANTIC_LOAD_ADDRESS + 0x5144)
    put_u32(0x50A4, SEMANTIC_LOAD_ADDRESS + 0x50C0)

    put_u16(0x50C0, 0x000B)
    put_u16(0x50C2, 0x0009)
    # D is intentionally disconnected. Its balanced stack spill restores r0
    # and r15 while making it a real resolution root without cross-function
    # dependencies, so a targeted change must retain this artifact exactly.
    put_u16(0x5100, 0x2F06)  # mov.l r0,@-r15
    put_u16(0x5102, 0x60F6)  # mov.l @r15+,r0
    put_u16(0x5104, 0x000B)
    put_u16(0x5106, 0x0009)
    put_u32(0x5140, 0)
    put_u32(0x5144, 0)
    return bytes(output)


def semantic_contract() -> dict[str, Any]:
    return {
        "abi_stack_argument_slot": 0,
        "boundaries": [
            {"offset": 0x00, "size": SEMANTIC_ROOT_TAIL + 0x0E},
            {"offset": 0x5000, "size": 0x10},
            {"offset": 0x5040, "size": 0x10},
            {"offset": 0x5080, "size": 0x0E},
            {"offset": 0x50C0, "size": 0x04},
            {"offset": 0x5100, "size": 0x08},
        ],
        "call_chain": [0x00, 0x5000, 0x5040, 0x5080, 0x50C0],
        "heavy_prefix_blocks": SEMANTIC_HEAVY_BLOCKS,
        "indirect_call_sites": [
            SEMANTIC_ROOT_TAIL + 0x06,
            0x5008,
            0x5042,
            0x5048,
            0x5086,
        ],
        "load_address": f"0x{SEMANTIC_LOAD_ADDRESS:08x}",
        "persistent_store_functions": [0x5000, 0x5080],
        "resolution_targets": [0x5000, 0x5040, 0x5080, 0x50C0],
        "schema": "katana-stress-semantic-graph-v1",
        "scc": [0x5000, 0x5040],
        "size": SEMANTIC_SIZE,
        "unaffected_functions": [0x5100],
        "version": 1,
    }


def analysis_overrides(profile: Profile) -> bytes:
    lines = [
        "version = 2",
        "schema = katana-analysis-directives",
        "mode = override",
    ]
    lines.extend(
        f"function = 0x{BOOT_LOAD_ADDRESS + index * FUNCTION_SIZE:08X} 0x{FUNCTION_SIZE:08X}"
        for index in range(profile.function_count)
    )
    return ("\n".join(lines) + "\n").encode("ascii")


def seed_wave_for_root(profile: Profile, root_index: int) -> int:
    end = 0
    for wave_index, count in enumerate(profile.seed_wave_counts):
        end += count
        if root_index < end:
            return wave_index
    raise AssertionError("root is not covered by a seed wave")


def build_roots(profile: Profile) -> bytes:
    if sum(profile.seed_wave_counts) != profile.root_count:
        raise AssertionError("seed waves do not cover every root")
    output = bytearray(
        ROOT_HEADER.pack(
            ROOT_MAGIC,
            ROOT_SCHEMA_VERSION,
            profile.root_count,
            ROOT_RECORD.size,
            len(profile.seed_wave_counts),
        )
    )
    for root_index in range(profile.root_count):
        wave = seed_wave_for_root(profile, root_index)
        output.extend(
            ROOT_RECORD.pack(
                root_index,
                (root_index * 37) % profile.function_count,
                wave,
                BLOCKS_PER_FUNCTION,
                0,
            )
        )
    return bytes(output)


def workload_contract(profile: Profile) -> dict[str, Any]:
    return {
        "block_count": profile.function_count * BLOCKS_PER_FUNCTION,
        "blocks_per_function": BLOCKS_PER_FUNCTION,
        "chunk_size": profile.chunk_size,
        "chunks_per_module": profile.chunks_per_module,
        "declared_entry_count": profile.module_count + 1,
        "declared_hint_count": profile.module_count + 3,
        "declared_source_binding_count": profile.module_count + 1,
        "cfa_delta_scale_function_counts": DELTA_SCALE_FUNCTION_COUNTS,
        "fva_delta_scale_function_counts": DELTA_SCALE_FUNCTION_COUNTS,
        "function_count": profile.function_count,
        "module_count": profile.module_count,
        "module_extent_count": profile.module_count + 1,
        "partition_count": profile.partition_count,
        "replay_passes": profile.replay_passes,
        "resolution_root_count": profile.root_count,
        "resolution_root_probe": "r0-stack-roundtrip",
        "persistent_physical_work_contract": "kr-4978-v1",
        "required_function_value_subphases": REQUIRED_FUNCTION_VALUE_SUBPHASES,
        "root_count": profile.root_count,
        "semantic_function_count": 6,
        "seed_wave_counts": list(profile.seed_wave_counts),
        "throughput_topology": "independent-bra-chains-with-resolution-roots",
        "wave_count": len(profile.seed_wave_counts),
    }


def build_partition_plan(profile: Profile) -> dict[str, Any]:
    partitions: list[dict[str, Any]] = []
    for partition_index in range(profile.partition_count):
        function_begin = partition_index * profile.function_count // profile.partition_count
        function_end = (partition_index + 1) * profile.function_count // profile.partition_count
        function_count = function_end - function_begin
        partition = {
            "block_count": function_count * BLOCKS_PER_FUNCTION,
            "function_begin": function_begin,
            "function_count": function_count,
            "function_end": function_end,
            "index": partition_index,
        }
        partitions.append(partition)
    return {
        "block_count": profile.function_count * BLOCKS_PER_FUNCTION,
        "blocks_per_function": BLOCKS_PER_FUNCTION,
        "function_count": profile.function_count,
        "partition_count": profile.partition_count,
        "partitions": partitions,
        "profile": profile.name,
        "schema": "katana-stress-partition-plan-v2",
        "version": 2,
    }


def build_module(profile: Profile, module_index: int) -> bytes:
    byte_size = profile.chunks_per_module * profile.chunk_size
    if byte_size % 4 != 0:
        raise AssertionError("module size must be a multiple of one synthetic block")
    block_count = byte_size // 4
    output = bytearray(byte_size)
    terminal_blocks = {block_count - 1}
    if module_index == 0:
        terminal_blocks.add(block_count // 2 - 1)
    for block_index in range(block_count):
        if block_index in terminal_blocks:
            struct.pack_into("<HH", output, block_index * 4, 0x000B, 0x0009)
            continue
        instruction = 0xE000 | (
            ((module_index + block_index) & 0x0F) << 8
        ) | ((module_index * 43 + block_index * 11 + DETERMINISTIC_SEED) & 0xFF)
        struct.pack_into("<HH", output, block_index * 4, 0xA000, instruction)
    return bytes(output)


def iso_readme(profile: Profile) -> bytes:
    return (
        "KATANA NATIVE DISC COLD BUILD STRESS FIXTURE\n"
        "PUBLIC, DETERMINISTIC, AND RETAIL-FREE\n"
        f"PROFILE={profile.name}\n"
        f"GENERATOR_SEED={DETERMINISTIC_SEED}\n"
        "ALL BYTES ARE SYNTHETIC AND REGENERABLE FROM THE REPOSITORY WRITER.\n"
    ).encode("ascii")


def build_ip_bin() -> bytes:
    ip_bin = bytearray(16 * LOGICAL_SECTOR_SIZE)
    ip_bin[0:16] = b"SEGA SEGAKATANA "
    ip_bin[0x25 : 0x25 + 9] = b"GD-ROM1/1"
    ip_bin[0x60 : 0x60 + 16] = b"BOOT.BIN".ljust(16, b" ")
    bootstrap = bytes(
        (
            0x01,
            0xD0,  # mov.l @(1,pc),r0
            0x2B,
            0x40,  # jmp @r0
            0x09,
            0x00,  # delay-slot nop
            0x09,
            0x00,  # aligned padding
            0x00,
            0x00,
            0x01,
            0x8C,  # 0x8C010000
        )
    )
    ip_bin[0x300 : 0x300 + len(bootstrap)] = bootstrap
    return bytes(ip_bin)


def build_iso(
    profile: Profile, payloads: list[tuple[str, bytes]]
) -> tuple[bytes, list[dict[str, Any]]]:
    dot_size = len(directory_record(DATA_LBA + 20, LOGICAL_SECTOR_SIZE, b"\0", True))
    parent_size = len(directory_record(DATA_LBA + 20, LOGICAL_SECTOR_SIZE, b"\1", True))
    record_sizes = [dot_size, parent_size]
    record_sizes.extend(
        len(directory_record(0, len(payload), name.encode("ascii"), False))
        for name, payload in payloads
    )
    root_byte_size = packed_directory_size(record_sizes)
    root_sector_count = root_byte_size // LOGICAL_SECTOR_SIZE
    next_sector = 20 + root_sector_count
    extents: list[tuple[str, bytes, int]] = []
    for name, payload in payloads:
        extent_sector = next_sector
        extents.append((name, payload, extent_sector))
        next_sector += (len(payload) + LOGICAL_SECTOR_SIZE - 1) // LOGICAL_SECTOR_SIZE
    logical_sector_count = next_sector
    image = bytearray(logical_sector_count * LOGICAL_SECTOR_SIZE)
    image[: 16 * LOGICAL_SECTOR_SIZE] = build_ip_bin()

    pvd_offset = 16 * LOGICAL_SECTOR_SIZE
    image[pvd_offset] = 1
    image[pvd_offset + 1 : pvd_offset + 6] = b"CD001"
    image[pvd_offset + 6] = 1
    image[pvd_offset + 8 : pvd_offset + 40] = b"KATANA".ljust(32, b" ")
    image[pvd_offset + 40 : pvd_offset + 72] = (
        f"KATANA_KR4974_{profile.name.upper()}".encode("ascii").ljust(32, b" ")
    )
    both_endian_u32(image, pvd_offset + 80, DATA_LBA + logical_sector_count)
    both_endian_u16(image, pvd_offset + 120, 1)
    both_endian_u16(image, pvd_offset + 124, 1)
    both_endian_u16(image, pvd_offset + 128, LOGICAL_SECTOR_SIZE)
    both_endian_u32(image, pvd_offset + 132, 10)
    image[pvd_offset + 140 : pvd_offset + 144] = (DATA_LBA + 18).to_bytes(4, "little")
    image[pvd_offset + 148 : pvd_offset + 152] = (DATA_LBA + 19).to_bytes(4, "big")
    root_record = directory_record(DATA_LBA + 20, root_byte_size, b"\0", True)
    image[pvd_offset + 156 : pvd_offset + 156 + len(root_record)] = root_record
    image[pvd_offset + 574 : pvd_offset + 702] = (
        b"KATANA RECOMP PUBLIC STRESS FIXTURE".ljust(128, b" ")
    )
    image[pvd_offset + 881] = 1

    terminator_offset = 17 * LOGICAL_SECTOR_SIZE
    image[terminator_offset] = 255
    image[terminator_offset + 1 : terminator_offset + 6] = b"CD001"
    image[terminator_offset + 6] = 1

    little_path = (
        b"\x01\x00"
        + (DATA_LBA + 20).to_bytes(4, "little")
        + (1).to_bytes(2, "little")
        + b"\x00\x00"
    )
    big_path = (
        b"\x01\x00"
        + (DATA_LBA + 20).to_bytes(4, "big")
        + (1).to_bytes(2, "big")
        + b"\x00\x00"
    )
    image[18 * LOGICAL_SECTOR_SIZE : 18 * LOGICAL_SECTOR_SIZE + 10] = little_path
    image[19 * LOGICAL_SECTOR_SIZE : 19 * LOGICAL_SECTOR_SIZE + 10] = big_path

    records = [
        directory_record(DATA_LBA + 20, root_byte_size, b"\0", True),
        directory_record(DATA_LBA + 20, root_byte_size, b"\1", True),
    ]
    records.extend(
        directory_record(
            DATA_LBA + extent_sector,
            len(payload),
            name.encode("ascii"),
            False,
        )
        for name, payload, extent_sector in extents
    )
    root_bytes = pack_directory(records, root_byte_size)
    root_offset = 20 * LOGICAL_SECTOR_SIZE
    image[root_offset : root_offset + root_byte_size] = root_bytes

    entries: list[dict[str, Any]] = []
    for name, payload, extent_sector in extents:
        offset = extent_sector * LOGICAL_SECTOR_SIZE
        image[offset : offset + len(payload)] = payload
        entries.append(
            {
                "extent_lba": DATA_LBA + extent_sector,
                "name": name,
                "sha256": sha256(payload),
                "size": len(payload),
            }
        )
    return bytes(image), sorted(entries, key=lambda entry: entry["name"])


def binary_coded_decimal(value: int) -> int:
    return ((value // 10) << 4) | (value % 10)


def mode1_track(logical_bytes: bytes, first_lba: int) -> bytes:
    if len(logical_bytes) % LOGICAL_SECTOR_SIZE != 0:
        raise AssertionError("logical track bytes are not sector aligned")
    sector_count = len(logical_bytes) // LOGICAL_SECTOR_SIZE
    output = bytearray(sector_count * RAW_SECTOR_SIZE)
    for sector_index in range(sector_count):
        raw_offset = sector_index * RAW_SECTOR_SIZE
        output[raw_offset : raw_offset + 12] = b"\x00" + b"\xFF" * 10 + b"\x00"
        absolute_frame = first_lba + sector_index + 150
        minute = (absolute_frame // (75 * 60)) % 100
        second = (absolute_frame // 75) % 60
        frame = absolute_frame % 75
        output[raw_offset + 12] = binary_coded_decimal(minute)
        output[raw_offset + 13] = binary_coded_decimal(second)
        output[raw_offset + 14] = binary_coded_decimal(frame)
        output[raw_offset + 15] = 1
        logical_offset = sector_index * LOGICAL_SECTOR_SIZE
        output[
            raw_offset + 16 : raw_offset + 16 + LOGICAL_SECTOR_SIZE
        ] = logical_bytes[logical_offset : logical_offset + LOGICAL_SECTOR_SIZE]
    return bytes(output)


def low_density_track() -> bytes:
    logical = bytearray(24 * LOGICAL_SECTOR_SIZE)
    marker = (
        b"KATANA RECOMP SYNTHETIC LOW-DENSITY TRACK; "
        b"NO RETAIL CONTENT; KR-4974 CONTRACT 1\n"
    )
    for sector_index in range(24):
        offset = sector_index * LOGICAL_SECTOR_SIZE
        logical[offset : offset + len(marker)] = marker
        logical[offset + 128 : offset + 132] = sector_index.to_bytes(4, "little")
    return mode1_track(bytes(logical), 0)


def synthetic_audio_track() -> bytes:
    output = bytearray(4 * RAW_SECTOR_SIZE)
    state = DETERMINISTIC_SEED
    for index in range(len(output)):
        state = (state * 1_664_525 + 1_013_904_223) & 0xFFFF_FFFF
        output[index] = (state >> 24) & 0xFF
    return bytes(output)


def gdi_descriptor() -> bytes:
    return (
        "3\n"
        "1 0 4 2352 track01.bin 0\n"
        "2 30 0 2352 track02.raw 0\n"
        "3 45000 4 2352 track03.bin 0\n"
    ).encode("ascii")


def latent_aot_hints(
    iso_entries: list[dict[str, Any]], modules: list[tuple[str, bytes]]
) -> bytes:
    entries_by_name = {entry["name"]: entry for entry in iso_entries}
    hints = []
    for name, module in modules:
        entry = entries_by_name[name]
        entry_offsets = (
            (0, len(module) // 2)
            if name in {"M000.BIN;1", "M000DUP.BIN;1"}
            else (0,)
        )
        for entry_offset in entry_offsets:
            hints.append(
                (
                    sha256(module),
                    entry["extent_lba"] * LOGICAL_SECTOR_SIZE,
                    len(module),
                    entry_offset,
                )
            )
    hints.sort()
    return "".join(
        f"sha256:{identity}@{disc_offset}:{byte_size}:{entry_offset}\n"
        for identity, disc_offset, byte_size, entry_offset in hints
    ).encode("ascii")


def prepare_output_directory(output: Path) -> None:
    if output.exists():
        if not output.is_dir():
            raise ValueError(f"output is not a directory: {output}")
        if any(output.iterdir()):
            raise ValueError(f"output directory must be empty: {output}")
        return
    output.mkdir(parents=True)


def generate(profile: Profile, output: Path) -> str:
    prepare_output_directory(output)
    workload = workload_contract(profile)
    partition_plan = build_partition_plan(profile)
    modules = [
        (f"M{module_index:03d}.BIN;1", build_module(profile, module_index))
        for module_index in range(profile.module_count)
    ]
    module_extents = [*modules, ("M000DUP.BIN;1", modules[0][1])]
    payloads = [
        ("BOOT.BIN;1", build_boot_program(profile)),
        ("ROOTS.BIN;1", build_roots(profile)),
        ("SEMANTIC.BIN;1", build_semantic_program()),
        ("SEMANTIC.JSN;1", canonical_json(semantic_contract())),
        (
            "WORKLOAD.JSN;1",
            canonical_json(
                {
                    "profile": profile.name,
                    "schema": WORKLOAD_SCHEMA,
                    "version": WORKLOAD_SCHEMA_VERSION,
                    "workload": workload,
                }
            ),
        ),
        ("PARTS.JSN;1", canonical_json(partition_plan)),
        ("README.TXT;1", iso_readme(profile)),
        *module_extents,
    ]

    print("[1/6] Synthetische ISO-Nutzdaten erzeugt.", flush=True)
    logical_track, iso_entries = build_iso(profile, payloads)
    track_bytes = {
        "track01.bin": low_density_track(),
        "track02.raw": synthetic_audio_track(),
        "track03.bin": mode1_track(logical_track, DATA_LBA),
    }
    print("[2/6] Gueltige Drei-Track-GDI aufgebaut.", flush=True)

    standalone_files = {
        "analysis.overrides": analysis_overrides(profile),
        "disc.gdi": gdi_descriptor(),
        "latent-aot-entries.txt": latent_aot_hints(iso_entries, module_extents),
        **track_bytes,
    }
    print("[3/6] Tracks und Analysevertraege vorbereitet.", flush=True)

    tracks = [
        {
            "lba": 0,
            "number": 1,
            "path": "track01.bin",
            "sector_size": RAW_SECTOR_SIZE,
            "sectors": len(track_bytes["track01.bin"]) // RAW_SECTOR_SIZE,
            "type": 4,
        },
        {
            "lba": 30,
            "number": 2,
            "path": "track02.raw",
            "sector_size": RAW_SECTOR_SIZE,
            "sectors": len(track_bytes["track02.raw"]) // RAW_SECTOR_SIZE,
            "type": 0,
        },
        {
            "lba": DATA_LBA,
            "number": 3,
            "path": "track03.bin",
            "sector_size": RAW_SECTOR_SIZE,
            "sectors": len(track_bytes["track03.bin"]) // RAW_SECTOR_SIZE,
            "type": 4,
        },
    ]
    artifacts = [
        {
            "path": name,
            "sha256": sha256(data),
            "size": len(data),
        }
        for name, data in sorted(standalone_files.items())
    ]
    manifest = {
        "artifacts": artifacts,
        "disc": {
            "boot_filename": "BOOT.BIN",
            "boot_load_address": f"0x{BOOT_LOAD_ADDRESS:08x}",
            "data_lba": DATA_LBA,
            "descriptor": "disc.gdi",
            "logical_sector_size": LOGICAL_SECTOR_SIZE,
            "raw_sector_size": RAW_SECTOR_SIZE,
            "tracks": tracks,
        },
        "formats": {
            "root_header_size": ROOT_HEADER.size,
            "root_magic": ROOT_MAGIC.decode("ascii"),
            "root_record_size": ROOT_RECORD.size,
        },
        "iso_files": iso_entries,
        "profile": profile.name,
        "provenance": {
            "contract_version": PROVENANCE_CONTRACT_VERSION,
            "deterministic_seed": DETERMINISTIC_SEED,
            "generator": "tools/performance/write_native_disc_cold_build_stress.py",
            "retail_content": False,
            "source_inputs": [],
        },
        "schema": MANIFEST_SCHEMA,
        "version": MANIFEST_SCHEMA_VERSION,
        "workload": workload,
    }
    manifest_bytes = canonical_json(manifest)
    manifest_digest = sha256(manifest_bytes)
    if manifest_digest != EXPECTED_MANIFEST_DIGESTS[profile.name]:
        raise AssertionError(
            "fixture contract drifted; review and pin manifest digest "
            f"{manifest_digest}"
        )
    for name in sorted(standalone_files):
        write_file(output / name, standalone_files[name])
    write_file(output / "stress-manifest.json", manifest_bytes)
    write_file(
        output / "stress-manifest.sha256",
        f"sha256:{manifest_digest}\n".encode("ascii"),
    )
    print("[4/6] Kanonisches Manifest und Digest geschrieben.", flush=True)
    print(
        f"[5/6] Profil {profile.name}: {workload['function_count']} Funktionen, "
        f"{workload['block_count']} Bloecke, {workload['root_count']} Roots, "
        f"{workload['wave_count']} echte Seedwellen, "
        f"{workload['module_count']} ExactOnly-Module.",
        flush=True,
    )
    print(f"[6/6] Fertig. Manifest-SHA-256: {manifest_digest}", flush=True)
    return manifest_digest


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a public deterministic NativeDisc cold-build stress fixture."
    )
    parser.add_argument("--profile", choices=sorted(PROFILES), required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        generate(PROFILES[arguments.profile], arguments.output.resolve())
    except (AssertionError, OSError, ValueError) as error:
        print(f"Fehler: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
