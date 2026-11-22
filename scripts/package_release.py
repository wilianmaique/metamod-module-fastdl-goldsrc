"""Package the Windows and Linux CMake installs from a shared staging directory."""

import argparse
from pathlib import Path
from tempfile import NamedTemporaryFile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


def package(stage, output):
    directory = Path("addons/fastdl")
    files = [directory / name for name in ("fastdl_mm.dll", "fastdl_mm.so", "fastdl.ini")]
    dll, so, ini = [(stage / name).read_bytes() for name in files]

    pe_offset = int.from_bytes(dll[60:64], "little")
    if dll[:2] != b"MZ" or dll[pe_offset:pe_offset + 6] != b"PE\0\0\x4c\x01":
        raise ValueError("fastdl_mm.dll must be a Windows x86 binary")
    if so[:6] != b"\x7fELF\x01\x01" or so[16:20] != b"\x03\x00\x03\x00":
        raise ValueError("fastdl_mm.so must be a Linux i386 shared object")
    if not ini.strip():
        raise ValueError("fastdl.ini is empty")

    output.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=output.parent, delete=False) as tmp:
        pending = Path(tmp.name)
    try:
        with ZipFile(pending, "w") as archive:
            for name, content in zip(files, (dll, so, ini)):
                entry = ZipInfo.from_file(stage / name, name.as_posix())
                entry.create_system = 3  # Store Unix permissions even when packaging on Windows.
                entry.external_attr = (0o100755 if name.suffix == ".so" else 0o100644) << 16
                archive.writestr(entry, content, compress_type=ZIP_DEFLATED)
        pending.replace(output)
    finally:
        pending.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path, help="common CMake install prefix for both platforms")
    parser.add_argument("output", type=Path, help="destination ZIP file")
    args = parser.parse_args()
    package(args.stage, args.output)
