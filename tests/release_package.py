"""Validate the Windows release archive, including its redistributable and controller."""
import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile
import zipfile


PE_MACHINES = {"x64": 0x8664, "arm64": 0xaa64, "x86": 0x14c}


def require_machine(stream, platform, name):
    dos = stream.read(64)
    if len(dos) != 64 or dos[:2] != b"MZ":
        raise ValueError(f"Invalid DOS header in {name}")
    stream.seek(struct.unpack_from("<I", dos, 60)[0])
    header = stream.read(6)
    if len(header) != 6 or header[:4] != b"PE\0\0":
        raise ValueError(f"Invalid PE header in {name}")
    machine = struct.unpack_from("<H", header, 4)[0]
    if machine != PE_MACHINES[platform]:
        raise ValueError(f"{name} has PE machine 0x{machine:04x}, expected {platform}")


def require_driver_models(data, platform):
    decoration = {"x64": "ntamd64", "arm64": "ntarm64", "x86": "ntx86"}[platform]
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    sections, models = set(), set()
    section = ""
    for raw in data.decode(encoding).splitlines():
        line = raw.partition(";")[0].strip().lower()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            sections.add(section)
        elif section == "manufacturer" and "=" in line:
            name, *targets = (field.strip() for field in line.split("=", 1)[1].split(","))
            models.update(f"{name}.{target}" for target in targets if target.split(".")[0] == decoration)
    if not sections.intersection(models):
        raise ValueError(f"Driver INF has no models for {platform}")


def verify(archive, platform, compiler_version):
    runtime = f"vc_redist.{platform}.exe"
    required = ["RustDeskIddApp.exe", runtime, "README.md"]
    required += [f"RustDeskIddDriver/RustDeskIddDriver.{ext}" for ext in ("inf", "dll", "cat")]
    with zipfile.ZipFile(archive) as package, tempfile.TemporaryDirectory() as work:
        names = {name.replace("\\", "/").lower(): name for name in package.namelist()}
        for name in required:
            if name.lower() not in names:
                raise ValueError(f"Missing from release: {name}")
        for name in ("RustDeskIddApp.exe", "RustDeskIddDriver/RustDeskIddDriver.dll"):
            with package.open(names[name.lower()]) as image:
                require_machine(image, platform, name)
        require_driver_models(package.read(names["rustdeskidddriver/rustdeskidddriver.inf"]), platform)
        for name in (runtime, "RustDeskIddApp.exe"):
            pathlib.Path(work, name).write_bytes(package.read(names[name.lower()]))
        check_runtime = pathlib.Path(__file__).with_name("verify-runtime.ps1")
        subprocess.run(["pwsh", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        str(check_runtime), str(pathlib.Path(work, runtime)), compiler_version],
                       check=True, timeout=60)
        subprocess.run([sys.executable, str(pathlib.Path(__file__).with_name("profile_cli.py")),
                        str(pathlib.Path(work, "RustDeskIddApp.exe"))], check=True, timeout=60)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=pathlib.Path)
    parser.add_argument("platform", choices=PE_MACHINES)
    parser.add_argument("compiler_version")
    parser.add_argument("--driver-tests", type=pathlib.Path)
    args = parser.parse_args()
    verify(args.archive, args.platform, args.compiler_version)
    if args.driver_tests:
        with args.driver_tests.open("rb") as image:
            require_machine(image, args.platform, args.driver_tests.name)
        subprocess.run([str(args.driver_tests.resolve())], check=True, timeout=60)
