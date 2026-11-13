"""Package source, configuration and documentation with a SHA256 manifest."""
from pathlib import Path
import argparse
import datetime as dt
import hashlib
import json
import zipfile

def main():
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=project.parent / "deliverables" / ("os-btl-adaptive-source-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S") + ".zip"))
    args = parser.parse_args()
    files = []
    for name in ("CMakeLists.txt", "sdkconfig", "sdkconfig.defaults", "README.md", "README_DEMO.md", "DESIGN_ADAPTIVE.md", "BUILD_VERIFICATION.md"):
        path = project / name
        if path.is_file():
            files.append(path)
    for folder in ("components", "main", "tools"):
        files.extend(path for path in (project / folder).rglob("*") if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc")
    hashes = {str(path.relative_to(project)).replace("\\", "/"): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "x", zipfile.ZIP_DEFLATED) as archive:
        for path in files:
            archive.write(path, "os-btl/" + path.relative_to(project).as_posix())
        archive.writestr("os-btl/SOURCE_SHA256.json", json.dumps(hashes, indent=2))
    with zipfile.ZipFile(args.out) as archive:
        if archive.testzip() is not None:
            raise RuntimeError("Archive verification failed")
    print(args.out)
    print(f"Source files: {len(files)}; archive bytes: {args.out.stat().st_size}")

if __name__ == "__main__":
    main()
