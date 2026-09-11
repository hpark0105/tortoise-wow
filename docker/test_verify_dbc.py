import hashlib
from pathlib import Path
import tempfile
import unittest

from verify_dbc import load_hashes, verify


class DbcCompatibilityChecks(unittest.TestCase):
    def test_detects_modified_and_missing_client_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = hashlib.sha256(b"original").hexdigest()
            (root / "Spell.dbc").write_bytes(b"original")
            self.assertEqual(verify(root, {"Spell.dbc": expected}), [])
            (root / "Spell.dbc").write_bytes(b"successor hotfix")
            self.assertEqual(verify(root, {"Spell.dbc": expected}), ["MISMATCH: Spell.dbc"])
            (root / "Spell.dbc").unlink()
            self.assertEqual(verify(root, {"Spell.dbc": expected}), ["MISSING: Spell.dbc"])

    def test_manifest_is_read_without_executing_its_script(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "verifier.py"
            manifest.write_text("EXPECTED_HASHES = {'Spell.dbc': 'abc', 'hashes.txt': 'def'}\nraise RuntimeError('do not execute')\n")
            self.assertEqual(load_hashes(manifest), {"Spell.dbc": "abc"})


if __name__ == "__main__":
    unittest.main()
