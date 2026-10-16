#!/usr/bin/env python3
"""
The Steam Frame's installer as release assets (port/android/STEAM_FRAME.md):

    python3 tools/steam_frame_bundle.py <apk> <output folder> <owner/repository>

writes into the output folder
- ringfall-mjolnir-vr-steam-frame.zip: the installer (port/android/steam_frame_install.sh,
  as install.sh), the APK (RingfallMjolnirVR.apk), the launcher
  (steam_frame_launch.sh, as ringfall-mjolnir-vr.sh, which updates the game
  from the repository named), the build's number (build.txt: the workflow's
  run number, HALO_BUILD_NUMBER or GITHUB_RUN_NUMBER; 0, which never
  updates, without one), the disc image reader (steam_frame_xiso.py, with
  extract-xiso's license) and the library pictures (grid/);
- Install-Ringfall-Mjolnir-VR.desktop (port/android/steam_frame_installer.desktop): what
  the player downloads and opens in Desktop Mode; it fetches the latest
  release's zip from the repository named.
"""

import os
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ANDROID = ROOT / "port/android"


def main():
    if len(sys.argv) != 4 or "/" not in sys.argv[3]:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    apk, output, repository = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    output.mkdir(parents=True, exist_ok=True)

    files = [
        (ANDROID / "steam_frame_install.sh", "install.sh", 0o755),
        (ANDROID / "steam_frame_launch.sh", "ringfall-mjolnir-vr.sh", 0o755),
        (ANDROID / "steam_frame_xiso.py", "steam_frame_xiso.py", 0o755),
        (ROOT / "port/third_party/extract-xiso/LICENSE.TXT", "extract-xiso-LICENSE.txt", 0o644),
        (apk, "RingfallMjolnirVR.apk", 0o644),
    ]
    files += [(picture, "grid/" + picture.name, 0o644)
        for picture in sorted((ANDROID / "steam_frame_grid").glob("*.png"))]
    build = os.environ.get("HALO_BUILD_NUMBER") or os.environ.get("GITHUB_RUN_NUMBER") or "0"
    if not build.isdigit():
        build = "0"
    bundle = output / "ringfall-mjolnir-vr-steam-frame.zip"
    with zipfile.ZipFile(bundle, "w", zipfile.ZIP_DEFLATED) as archive:
        for path, name, mode in files:
            info = zipfile.ZipInfo.from_file(path, name)
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_STORED if name.endswith((".apk", ".png")) else zipfile.ZIP_DEFLATED
            data = path.read_bytes()
            if name == "ringfall-mjolnir-vr.sh":
                data = data.replace(b"@REPOSITORY@", repository.encode())
            archive.writestr(info, data)
        info = zipfile.ZipInfo("build.txt", date_time=(2026, 1, 1, 0, 0, 0))
        info.external_attr = (0o100000 | 0o644) << 16
        archive.writestr(info, build + "\n")
    print(f"{bundle.relative_to(Path.cwd()) if bundle.is_relative_to(Path.cwd()) else bundle}")

    desktop = (ANDROID / "steam_frame_installer.desktop").read_text().replace("@REPOSITORY@", repository)
    installer = output / "Install-Ringfall-Mjolnir-VR.desktop"
    installer.write_text(desktop)
    installer.chmod(0o755)
    print(installer)
    return 0


if __name__ == "__main__":
    sys.exit(main())
