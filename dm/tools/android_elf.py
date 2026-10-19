"""Validate AArch64 shared-library inputs for Android packaging."""
import hashlib
import struct


def elf_info(data, label):
    """Check the target ABI and record each library's own ELF page alignment."""
    if (len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01" or
            struct.unpack_from("<HHI", data, 16) != (3, 183, 1)):
        raise ValueError(f"{label}: expected little-endian AArch64 ELF64 shared library")
    offset = struct.unpack_from("<Q", data, 32)[0]
    header_size, stride, count = struct.unpack_from("<HHH", data, 52)
    if header_size != 64 or stride < 56 or not count or offset < 64 or offset+count*stride > len(data):
        raise ValueError(f"{label}: invalid ELF64 program header table")
    loads = [struct.unpack_from("<IIQQQQQQ", data, offset+i*stride) for i in range(count)]
    loads = [item for item in loads if item[0] == 1]
    if not loads or any(item[2]+item[5] > len(data) or item[5] > item[6] for item in loads):
        raise ValueError(f"{label}: invalid ELF64 PT_LOAD segments")
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "elf_class": 64, "elf_machine": 183, "elf_endianness": "little",
            "pt_load_min_alignment": min(item[7] for item in loads),
            "elf_pt_load_16k_compatible": all(item[7] >= 16384 and (item[3]-item[2]) % 16384 == 0 for item in loads)}
