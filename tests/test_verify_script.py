from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class VerifyScriptTests(unittest.TestCase):
    def test_failed_git_fsck_produces_nonzero_exit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bin_dir = Path(temporary) / "bin"
            bin_dir.mkdir()
            fake_git = bin_dir / "git"
            fake_git.write_text(
                "#!/bin/sh\n"
                "case \"$1\" in\n"
                "  fsck) exit 23 ;;\n"
                "  status) exit 0 ;;\n"
                "  *) exit 0 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            fake_git.chmod(0o755)

            environment = os.environ.copy()
            environment["PATH"] = f"{bin_dir}{os.pathsep}{environment.get('PATH', '')}"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "verify.sh")],
                cwd=ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertIn("FAIL git fsck", result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failed_git_status_produces_nonzero_exit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bin_dir = Path(temporary) / "bin"
            bin_dir.mkdir()
            fake_git = bin_dir / "git"
            fake_git.write_text(
                "#!/bin/sh\n"
                "case \"$1\" in\n"
                "  fsck) exit 0 ;;\n"
                "  status) exit 23 ;;\n"
                "  *) exit 0 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            fake_git.chmod(0o755)

            environment = os.environ.copy()
            environment["PATH"] = f"{bin_dir}{os.pathsep}{environment.get('PATH', '')}"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "verify.sh")],
                cwd=ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
