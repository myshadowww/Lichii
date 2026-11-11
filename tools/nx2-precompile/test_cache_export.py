#!/usr/bin/env python3
"""Exercise cache-export with synthetic logs; set NX2_TOOL to the built tool.

Requires the xxhash shared library to construct independently checksummed records.
No game files or recorded shaders are used.
"""
import ctypes
import ctypes.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib


class CacheExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = os.environ.get('NX2_TOOL', 'nx2-precompile')
        cls.version = int(subprocess.check_output([cls.tool, 'cache-version'], text=True))
        library = ctypes.util.find_library('xxhash')
        if not library:
            raise RuntimeError('xxhash shared library is required for the fixture writer')
        cls.xxhash = ctypes.CDLL(library)
        cls.xxhash.XXH3_64bits_withSeed.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                                 ctypes.c_uint64]
        cls.xxhash.XXH3_64bits_withSeed.restype = ctypes.c_uint64

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name) / 'input.v2.bin'
        self.output = Path(self.directory.name) / 'export.bin'

    @staticmethod
    def header(version=2):
        prefix = struct.pack('<8sIIQ', b'LXSHCACH', version, 32, 0)
        return prefix + struct.pack('<II', zlib.crc32(prefix), 0)

    def record(self, kind, key, payload):
        prefix = struct.pack('<IHHIQ', 0x5243584c, kind, 0, len(payload), key)
        checksum = self.xxhash.XXH3_64bits_withSeed(payload, len(payload), key ^ (kind << 48))
        record = prefix + struct.pack('<I', zlib.crc32(prefix)) + payload
        record += struct.pack('<Q', checksum)
        return record + bytes(-len(record) % 8)

    def export(self, data, output=None):
        self.source.write_bytes(data)
        result = subprocess.run([self.tool, 'cache-export', '--cache', str(self.source),
                                 '--out', str(output or self.output)],
                                capture_output=True, text=True)
        self.assertEqual(self.source.read_bytes(), data, 'source cache was modified')
        return result

    def legacy(self, payload):
        return b'yuzucach' + struct.pack('<I', self.version) + payload

    def test_only_latest_live_pipeline_records_are_exported(self):
        data = self.header()
        data += self.record(2, 10, b'old graphics')
        data += self.record(3, 20, b'compute')
        data += self.record(2, 10, b'new graphics')
        data += self.record(2, 30, b'deleted graphics')
        data += self.record(4, 30, struct.pack('<H', 2))
        data += self.record(1, 40, b'program metadata')
        data += self.record(5, 50, b'used list')
        result = self.export(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_bytes(), self.legacy(b'computenew graphics'))
        self.assertIn('1 graphics and 1 compute', result.stdout)
        self.assertIn('1 tombstones', result.stdout)

    def test_corrupt_payload_and_partial_tail_are_reported_and_ignored(self):
        damaged = bytearray(self.record(2, 10, b'bad graphics'))
        damaged[24] ^= 1
        data = self.header() + damaged + self.record(3, 20, b'good compute')
        data += self.record(2, 30, b'incomplete')[:-10]
        result = self.export(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_bytes(), self.legacy(b'good compute'))
        self.assertIn('1 damaged records', result.stdout)
        self.assertNotIn('0 tail bytes ignored', result.stdout)

    def test_invalid_headers_do_not_replace_existing_output(self):
        for data in [b'', b'not a cache', self.header(3), bytes(32), self.header()[:24]]:
            with self.subTest(header=data):
                self.output.write_bytes(b'keep this output')
                result = self.export(data)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.output.read_bytes(), b'keep this output')

    def test_empty_valid_log_is_distinct_from_an_invalid_log(self):
        result = self.export(self.header())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_bytes(), self.legacy(b''))

    def test_export_cannot_overwrite_the_source_or_an_alias(self):
        data = self.header() + self.record(2, 10, b'graphics')
        self.source.write_bytes(data)
        symlink = self.source.with_name('symlink.bin')
        symlink.symlink_to(self.source)
        hardlink = self.source.with_name('hardlink.bin')
        hardlink.hardlink_to(self.source)
        for path in [self.source, symlink, hardlink]:
            with self.subTest(path=path):
                result = self.export(data, path)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('must differ', result.stderr)


if __name__ == '__main__':
    unittest.main()
