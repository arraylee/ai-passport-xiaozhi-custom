"""Run the shipped portal JavaScript; no firmware/device or network is contacted."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PortalPageTest(unittest.TestCase):
    def test_shipped_form(self):
        subprocess.run(
            ['node', str(ROOT / 'tests/host/portal_page_harness.cjs'),
             str(ROOT / 'components/platform/xiaozhi/portal.html')],
            check=True,
            # Node startup can exceed ten seconds when concurrent firmware builds run.
            timeout=30,
        )


if __name__ == '__main__':
    unittest.main()
