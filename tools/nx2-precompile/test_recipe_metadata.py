#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Recipe creation must not produce files that the desktop loader cannot use."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class RecipeMetadataTests(unittest.TestCase):
    def run_recipe(self, with_program_id):
        tool = os.environ.get('NX2_TOOL', 'nx2-precompile')
        version = int(subprocess.check_output([tool, 'cache-version'], text=True))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'romfs').mkdir()
            if with_program_id:
                (root / 'exefs').mkdir()
                npdm = bytearray(160)
                struct.pack_into('<I', npdm, 0x70, 0x80)
                npdm[0x80:0x84] = b'ACI0'
                struct.pack_into('<Q', npdm, 0x90, 0x0400000000000000)
                (root / 'exefs/main.npdm').write_bytes(npdm)
            prep = root / 'empty.nx2prep'
            prep.write_bytes(struct.pack('<8sIIII', b'NX2PREP1', version, 0, 0, 0))
            recipe = root / 'game.nx2recipe'
            recipe.write_bytes(b'previous valid artifact')
            run = subprocess.run([tool, 'recipe', '--game', str(root / 'romfs'),
                                  '--file', str(prep), '--out', str(recipe), '--threads', '1'],
                                 capture_output=True, text=True, timeout=20)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(recipe.read_bytes(), b'previous valid artifact')
            return run.stderr

    def test_missing_program_id_is_rejected(self):
        self.assertIn('program ID', self.run_recipe(False))

    def test_empty_recipe_is_rejected(self):
        self.assertIn('none of the pipelines', self.run_recipe(True))


if __name__ == '__main__':
    unittest.main()
