import argparse
import os
import shutil
import struct
import sys
from pathlib import Path


def unfself(src_path: Path, dst_path: Path) -> bool:
    with open(src_path, "rb") as f_in:
        magic = f_in.read(4)
        if magic not in (b"\x4F\x15\x3D\x1D", b"\x54\x14\xF5\xEE"):
            if magic == b"\x7FELF":
                f_in.seek(0)
                with open(dst_path, "wb") as f_out:
                    shutil.copyfileobj(f_in, f_out)
                return True
            raise ValueError(f"Unknown magic {magic!r} in {src_path}")
        f_in.seek(8)
        ext_fmt = "<2B2x2HQ2H4x"
        content_type, key_type, header_size, meta_size, file_size, entry_count, flag = struct.unpack(
            ext_fmt, f_in.read(struct.calcsize(ext_fmt))
        )
        entries = []
        entry_fmt = "<4Q"
        last_file_offset_end = 0
        for _ in range(entry_count):
            props, file_offset, fsize, mem_size = struct.unpack(
                entry_fmt, f_in.read(struct.calcsize(entry_fmt))
            )
            orig = f_in.tell()
            f_in.seek(file_offset)
            data = f_in.read(fsize)
            entries.append((fsize, data))
            last_file_offset_end = max(last_file_offset_end, file_offset + fsize)
            f_in.seek(orig)
        f_in.seek(last_file_offset_end)
        tail_data = f_in.read()
        f_in.seek(0x1A0)
        elf_base_fmt = "<4s5B6xB"
        magic, arch, endian, ver, os_abi, abi_ver, eid_size = struct.unpack(
            elf_base_fmt, f_in.read(struct.calcsize(elf_base_fmt))
        )
        ex_fmt = "<2HI3QI6H"
        ex_data = struct.unpack(ex_fmt, f_in.read(struct.calcsize(ex_fmt)))
        prog_hdr_count = ex_data[9]
        ph_fmt = "<2I6Q"
        ph_size = struct.calcsize(ph_fmt)
        ph_list = []
        max_file_end = 0
        for _ in range(prog_hdr_count):
            ph_fields = struct.unpack(ph_fmt, f_in.read(ph_size))
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = ph_fields
            ph_list.append(ph_fields)
            max_file_end = max(max_file_end, p_offset + p_filesz)
        dst_path.parent.mkdir(parents=True, exist_ok=True)
        with open(dst_path, "wb") as f_out:
            if max_file_end > 0:
                f_out.truncate(max_file_end)
            f_out.seek(0)
            f_out.write(struct.pack(elf_base_fmt, magic, arch, endian, ver, os_abi, abi_ver, eid_size))
            f_out.write(struct.pack(ex_fmt, *ex_data))
            for ph in ph_list:
                f_out.write(struct.pack(ph_fmt, *ph))
            entries_copy = list(entries)
            for ph in ph_list:
                p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = ph
                for i, (sz, data) in enumerate(entries_copy):
                    if sz == p_filesz and sz > 0:
                        f_out.seek(p_offset)
                        f_out.write(data)
                        entries_copy.pop(i)
                        break
            if tail_data:
                for ph in ph_list:
                    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = ph
                    if p_filesz == len(tail_data):
                        f_out.seek(p_offset)
                        f_out.write(tail_data)
                        break
            if f_out.seek(0, os.SEEK_END) < max_file_end:
                f_out.truncate(max_file_end)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert PS4/PS5 SELF/FSELF binaries to standard ELF")
    parser.add_argument("input", type=Path, help="Input SELF/FSELF file or directory")
    parser.add_argument("output", type=Path, nargs="?", help="Output ELF file or directory")
    args = parser.parse_args()

    if args.input.is_dir():
        out_dir = args.output if args.output else args.input
        count = 0
        for root, _, files in os.walk(args.input):
            for file in files:
                src = Path(root) / file
                rel = src.relative_to(args.input)
                dst = out_dir / rel
                try:
                    with open(src, "rb") as probe:
                        hdr = probe.read(4)
                    if hdr in (b"\x4F\x15\x3D\x1D", b"\x54\x14\xF5\xEE"):
                        unfself(src, dst)
                        count += 1
                except Exception:
                    pass
        print(f"Extracted {count} SELF binaries to {out_dir}")
        return 0

    out_file = args.output if args.output else args.input.with_suffix(".elf")
    unfself(args.input, out_file)
    print(f"Extracted {args.input} -> {out_file}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
