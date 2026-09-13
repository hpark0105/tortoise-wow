"""Fail on missing/mismatched DBCs using the repository's existing hash list."""
import argparse
import ast
import hashlib
from pathlib import Path


def load_hashes(manifest):
    for node in ast.parse(manifest.read_text(encoding="utf-8")).body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "EXPECTED_HASHES"
            for target in node.targets
        ):
            return {name: digest for name, digest in ast.literal_eval(node.value).items()
                    if name.lower().endswith(".dbc")}
    raise ValueError("No EXPECTED_HASHES dictionary in the repository verifier")


def verify(directory, hashes):
    errors = []
    for name, expected in hashes.items():
        path = directory / name
        if not path.is_file():
            errors.append(f"MISSING: {name}")
            continue
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != expected:
            errors.append(f"MISMATCH: {name}")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    hashes = load_hashes(args.manifest)
    if not hashes:
        parser.error("Manifest contains no DBC hashes")
    errors = verify(args.directory, hashes)
    for error in errors:
        print(error)
    print(f"Verified {len(hashes) - len(errors)}/{len(hashes)} DBC files.")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
