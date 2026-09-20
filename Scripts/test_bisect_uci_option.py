import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().with_name("bisect_uci_option.py")
START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"


def run(*args, cwd=None):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )


class BisectUciOptionTests(unittest.TestCase):
    def test_self_test_passes_from_another_working_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            result = run("--self-test", cwd=temp_dir)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("[FAIL]", result.stdout)

    def test_self_check_accepts_a_legal_corpus(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            corpus = Path(temp_dir) / "corpus.jsonl"
            corpus.write_text(
                json.dumps({"id": 1, "fen": START, "expect": "e2e4"}) + "\n" + START + "\n",
                encoding="ascii",
            )

            result = run(str(corpus), "--self-check", cwd=temp_dir)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("[FAIL]", result.stdout)

    def test_self_check_rejects_an_illegal_expect(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            corpus = Path(temp_dir) / "corpus.jsonl"
            corpus.write_text(
                json.dumps({"id": 1, "fen": START, "expect": "e2e5"}) + "\n", encoding="ascii"
            )

            result = run(str(corpus), "--self-check", cwd=temp_dir)

            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("[FAIL] every expect legal in its FEN", result.stdout)

    def test_an_engineless_invocation_is_refused_rather_than_measuring_nothing(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            corpus = Path(temp_dir) / "corpus.fen"
            corpus.write_text(START + "\n", encoding="ascii")

            result = run(str(corpus), cwd=temp_dir)

            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("--engine", result.stderr)


if __name__ == "__main__":
    unittest.main()
