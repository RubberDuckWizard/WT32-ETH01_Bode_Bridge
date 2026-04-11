from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BUILD_ROOT = PROJECT_ROOT / ".pio" / "build"
RELEASE_ROOT = PROJECT_ROOT / "release"
ENV_OUTPUT_LAYOUT = {
    "wt32eth_release_final_safe": {
        "dir": "wt32eth_release_final_safe",
        "files": {
            "firmware.bin": "app.bin",
            "bootloader.bin": "bootloader.bin",
            "partitions.bin": "partitions.bin",
        },
    },
    "wt32eth_bringup_safe": {
        "dir": "wt32eth_bringup_safe",
        "files": {
            "firmware.bin": "app.bin",
        },
    },
    "wt32eth_final_test_safe": {
        "dir": "wt32eth_final_test_safe",
        "files": {
            "firmware.bin": "app.bin",
        },
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the kept firmware variants and copy release-ready binaries into the release directory."
    )
    parser.add_argument(
        "-e",
        "--env",
        dest="envs",
        action="append",
        choices=sorted(ENV_OUTPUT_LAYOUT),
        help="PlatformIO environment to build. Repeat the option to build more than one environment.",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Run a clean build for each selected environment.",
    )
    return parser.parse_args()


def resolve_platformio_command() -> list[str]:
    candidates = [
        [sys.executable, "-m", "platformio"],
        ["platformio"],
        ["pio"],
    ]

    for candidate in candidates:
        try:
            completed = subprocess.run(
                [*candidate, "--version"],
                cwd=PROJECT_ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
        except OSError:
            continue

        if completed.returncode == 0:
            return candidate

    raise RuntimeError(
        "PlatformIO was not found. Install it with 'pip install platformio' or run the script from an environment where PlatformIO is available."
    )


def run_command(command: list[str]) -> None:
    print(" ".join(command))
    subprocess.run(command, cwd=PROJECT_ROOT, check=True)


def sha256sum(file_path: Path) -> str:
    digest = hashlib.sha256()
    with file_path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_environment(platformio_cmd: list[str], env_name: str, clean: bool) -> list[Path]:
    if clean:
        run_command([*platformio_cmd, "run", "-d", str(PROJECT_ROOT), "-e", env_name, "-t", "clean"])

    run_command([*platformio_cmd, "run", "-d", str(PROJECT_ROOT), "-e", env_name])

    layout = ENV_OUTPUT_LAYOUT[env_name]
    destination_dir = RELEASE_ROOT / layout["dir"]
    destination_dir.mkdir(parents=True, exist_ok=True)

    copied_files: list[Path] = []
    for source_name, destination_name in layout["files"].items():
        source_file = BUILD_ROOT / env_name / source_name
        if not source_file.is_file():
            raise FileNotFoundError(f"Generated artifact not found: {source_file}")

        destination_file = destination_dir / destination_name
        shutil.copy2(source_file, destination_file)
        print(f"Copied: {source_file} -> {destination_file}")
        copied_files.append(destination_file)

    return copied_files


def write_checksums(files: list[Path]) -> Path:
    RELEASE_ROOT.mkdir(parents=True, exist_ok=True)
    checksum_file = RELEASE_ROOT / "SHA256SUMS.txt"
    with checksum_file.open("w", encoding="utf-8", newline="\n") as handle:
        for file_path in sorted(files):
            relative_path = file_path.relative_to(PROJECT_ROOT).as_posix()
            handle.write(f"{sha256sum(file_path)}  {relative_path}\n")
    return checksum_file


def main() -> int:
    args = parse_args()
    envs = args.envs or list(ENV_OUTPUT_LAYOUT)

    try:
        platformio_cmd = resolve_platformio_command()
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 1

    try:
        copied_files: list[Path] = []
        for env_name in envs:
            copied_files.extend(build_environment(platformio_cmd, env_name, args.clean))
        checksum_file = write_checksums(copied_files)
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    print("Build completed. Generated release files:")
    for file_path in copied_files:
        print(f"- {file_path.relative_to(PROJECT_ROOT).as_posix()}")
    print(f"- {checksum_file.relative_to(PROJECT_ROOT).as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
