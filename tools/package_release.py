"""Package an EXE only with a recent, matching Microsoft Defender scan receipt.

The receipt is build metadata, not a signature or an independent security verdict.
This prevents accidentally publishing the withdrawn or an unscanned local build.
"""
import argparse
from datetime import datetime, timezone, timedelta
import hashlib
import json
from pathlib import Path
import zipfile

WITHDRAWN_SHA256 = {"0824f6ec56eabcfbc8533bda20b5b3de21c68c2280958650a83e5f40dd99f1eb"}


def check_scan(exe, report_path):
    data = exe.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError("Input must be a built Windows executable")
    digest = hashlib.sha256(data).hexdigest()
    if digest in WITHDRAWN_SHA256:
        raise ValueError("This executable was withdrawn after a Defender detection; it cannot be packaged")
    report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    if (report.get("schema_version") != 1 or report.get("scanner") != "Microsoft Defender"
            or report.get("result") != "no_threats_detected"):
        raise ValueError("A successful Microsoft Defender scan receipt is required")
    finished = datetime.fromisoformat(report["finished_at"].replace("Z", "+00:00"))
    if finished.tzinfo is None:
        raise ValueError("The scan receipt date must include a timezone")
    age = datetime.now(timezone.utc) - finished
    if age < -timedelta(minutes=5) or age > timedelta(hours=24):
        raise ValueError("The scan receipt must be dated within the last 24 hours")
    if not report.get("signature_version") or not report.get("engine_version"):
        raise ValueError("The scan receipt does not identify Defender and its signatures")
    if not any(item.get("sha256", "").lower() == digest and item.get("size") == len(data)
               for item in report.get("files", [])):
        raise ValueError("The EXE does not match any file in the scan receipt")
    return data


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=root / "build-msvc" / "Release" / "Sifa.exe")
    parser.add_argument("--scan-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=root / "build-msvc" / "Sifa-Windows-preview.zip")
    args = parser.parse_args()
    try:
        executable = check_scan(args.exe, args.scan_report)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        # Package the checked bytes, even if the source file changes later.
        archive.writestr("Sifa/Sifa.exe", executable)
        archive.write(args.scan_report, "Sifa/defender-scan.json")
        for file in ["README.md", "docs/architecture.md", "docs/validation.md",
                     "include/third_party/JSON_LICENSE.MIT"]:
            archive.write(root / file, "Sifa/" + file)
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    checksum = args.output.with_suffix(args.output.suffix + ".sha256")
    checksum.write_text(f"{digest}  {args.output.name}\n", encoding="utf-8")
    print(args.output)
    print(f"SHA256 {digest}")


if __name__ == "__main__":
    main()
