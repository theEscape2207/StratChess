import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().with_name("build_corpus.py")


class BuildCorpusTests(unittest.TestCase):
    def test_default_root_harvests_repository_assets_from_another_working_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "corpus.fen"
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--out", str(output)],
                cwd=temp_dir,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.exists())
            positions = [line for line in output.read_text(encoding="ascii").splitlines() if line.strip()]
            self.assertGreater(len(positions), 0)


if __name__ == "__main__":
    unittest.main()
