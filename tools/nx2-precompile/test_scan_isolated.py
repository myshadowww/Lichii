#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic format, ordinal merge, subprocess failure and timeout checks."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / 'py'))
import scan_isolated as scan


def package(fingerprints):
    data = bytearray(b'NXSX0001' + struct.pack('<I', len(fingerprints)))
    for fingerprint in fingerprints:
        data += struct.pack('<BQIII', 0, fingerprint, 4, 8, 0) + bytes(12)
    return data


FAKE = '''#!/usr/bin/env python3
import resource, struct, sys, time
from pathlib import Path
args = dict(zip(sys.argv[2::2], sys.argv[3::2]))
assert resource.getrlimit(resource.RLIMIT_AS) == (128 * 1024 * 1024,) * 2
assert resource.getrlimit(resource.RLIMIT_CORE) == (0, 0)
data = Path(args['--pkg']).read_bytes()
n = struct.unpack_from('<I', data, 8)[0]
fps = [struct.unpack_from('<Q', data, 13 + 33 * i)[0] for i in range(n)]
if 102 in fps:
    sys.exit(7)
if 999 in fps:
    time.sleep(5)
out = Path(args['--out'])
header = 'file\\tindex\\tstage\\tstatus\\tcode_hash\\tnative_key\\tcode_words\\tenv_bytes\\tqueries\\tgeneric_loads\\theader_reads\\terror\\n'
rows = [f'programs\\t{i}\\tVertexB\\tok\\t{fp * 2:016x}\\t0000000000000000\\t1\\t4\\t0\\t00000000\\t00000000\\t\\n' for i,fp in enumerate(fps)]
Path(str(out) + '.tsv').write_text(header + ''.join(rows))
blob = bytearray(b'NXPENV01')
for i,fp in enumerate(fps):
    blob += struct.pack('<IQII', i, fp * 2, 4, fp)
Path(str(out) + '.envs').write_bytes(blob)
'''


class IsolatedScanTests(unittest.TestCase):
    def test_rejects_truncated_or_unknown_programs(self):
        valid = package([100])
        self.assertEqual(scan.read_programs(valid)[0].fingerprint, 100)
        for invalid in (b'', valid[:-1], valid + b'x', valid[:12] + bytes([9]) + valid[13:]):
            with self.subTest(size=len(invalid)):
                with self.assertRaises(ValueError):
                    scan.read_programs(invalid)

    def run_scan(self, directory, fingerprints, timeout=2, tool_in_path=False):
        root = Path(directory)
        tool = root / 'fake-tool'
        tool.write_text(FAKE)
        tool.chmod(0o755)
        pkg = root / 'game.programs'
        pkg.write_bytes(package(fingerprints))
        prefix = root / 'result'
        result = subprocess.run([sys.executable, '-I', str(HERE / 'py/scan_isolated.py'),
                                 '--tool', tool.name if tool_in_path else str(tool),
                                 '--pkg', str(pkg), '--out', str(prefix),
                                 '--batch-size', '3', '--workers', '2', '--memory-mib', '128',
                                 '--timeout', str(timeout)], capture_output=True, text=True, timeout=15,
                                env=dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH']))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return prefix

    def test_tool_can_be_resolved_on_path(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = self.run_scan(directory, [100], tool_in_path=True)
            report = json.loads(Path(str(prefix) + '.scan.json').read_text())
            self.assertEqual(report['status_counts'], {'ok': 1})

    def test_merge_keeps_global_ordinals_and_reports_failed_program(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = self.run_scan(directory, [100, 101, 102, 103, 104])
            rows = prefix.with_suffix('.tsv').read_text().splitlines()[1:]
            self.assertEqual([int(row.split('\t')[1]) for row in rows], list(range(5)))
            self.assertEqual(rows[2].split('\t')[3], 'isolated-scan-failed')
            data = prefix.with_suffix('.envs').read_bytes()
            self.assertEqual(data[:8], b'NXPENV01')
            envs = [struct.unpack_from('<IQII', data, pos) for pos in range(8, len(data), 20)]
            self.assertEqual(envs, [(0, 200, 4, 100), (1, 202, 4, 101),
                                   (3, 206, 4, 103), (4, 208, 4, 104)])
            report = json.loads(Path(str(prefix) + '.scan.json').read_text())
            self.assertEqual(report['failed'][0]['fingerprint'], '0000000000000066')
            self.assertEqual(report['failed'][0]['ordinal'], 2)
            self.assertTrue(any(row['status'] == 7 for row in report['attempts']))
            self.assertTrue((Path(str(prefix) + '.scan-batches') /
                             '00000002-00000002.programs').exists())

    def test_timeout_is_explicit_and_does_not_write_an_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = self.run_scan(directory, [999], timeout=0.2)
            report = json.loads(Path(str(prefix) + '.scan.json').read_text())
            self.assertIn('timeout', report['failed'][0]['error'])
            self.assertEqual(prefix.with_suffix('.envs').read_bytes(), b'NXPENV01')

    def test_merge_rejects_duplicate_environment_ordinals(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / 'scan'
            prefix.with_suffix('.tsv').write_text(scan.HEADER +
                'programs\t0\tVertexB\tok\t1\t0\t0\t4\t0\t0\t0\t\n')
            prefix.with_suffix('.envs').write_bytes(b'NXPENV01' + struct.pack('<IQII', 0, 1, 4, 7) * 2)
            with self.assertRaises(ValueError):
                scan.remap_outputs(prefix, [10])

    def test_merge_rejects_missing_or_mismatched_environments(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / 'scan'
            prefix.with_suffix('.tsv').write_text(scan.HEADER +
                'programs\t0\tVertexB\tok\t1\t0\t0\t4\t0\t0\t0\t\n')
            for blob in (b'NXPENV01', b'NXPENV01' + struct.pack('<IQII', 0, 2, 4, 7)):
                prefix.with_suffix('.envs').write_bytes(blob)
                with self.assertRaises(ValueError):
                    scan.remap_outputs(prefix, [10])
            prefix.with_suffix('.tsv').write_text(scan.HEADER +
                'programs\t0\tVertexB\ttranslate\t1\t0\t0\t4\t0\t0\t0\terror\n')
            prefix.with_suffix('.envs').write_bytes(b'NXPENV01' + struct.pack('<IQII', 0, 1, 4, 7))
            with self.assertRaises(ValueError):
                scan.remap_outputs(prefix, [10])


if __name__ == '__main__':
    unittest.main()
