from pathlib import Path


FIXTURES = {
    "register_displacement_execution.bin": """
        1F 80 2F 81 3F 14 5F 84 6F 85 7F 58 0B 00 09 00
    """,
    "r0_indexed_execution.bin": """
        14 02 35 04 56 06 7C 08 9D 0A BE 0C 04 0D EC 00
        0B 00 09 00
    """,
    "pc_relative_execution.bin": """
        02 91 03 D2 09 00 04 C7 0B 00 09 00
    """,
}


def gbr_fixture() -> bytes:
    data = bytearray.fromhex("09 00" * 43)
    opcodes = {
        0x00: 0xB016,
        0x04: 0xB01C,
        0x08: 0xB01E,
        0x0C: 0xB020,
        0x10: 0x000B,
        0x30: 0xC0FF,
        0x32: 0xC1FF,
        0x34: 0xC2FF,
        0x36: 0x000B,
        0x40: 0xC4FF,
        0x42: 0x000B,
        0x48: 0xC5FF,
        0x4A: 0x000B,
        0x50: 0xC6FF,
        0x52: 0x000B,
    }
    for offset, opcode in opcodes.items():
        data[offset : offset + 2] = opcode.to_bytes(2, "little")
    return bytes(data)


def main() -> None:
    fixture_dir = Path(__file__).resolve().parents[1] / "tests" / "fixtures"
    for name, source in FIXTURES.items():
        (fixture_dir / name).write_bytes(bytes.fromhex(source))
    (fixture_dir / "gbr_displacement_execution.bin").write_bytes(gbr_fixture())


if __name__ == "__main__":
    main()
