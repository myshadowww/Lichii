#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic Eclipse ZIP/LZ4 shader extraction checks; pass nx2-precompile as argv[1]."""
import ctypes
import ctypes.util
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

TOOL = pathlib.Path(sys.argv.pop(1)).resolve()


def shader(code_size=512):
    code = bytearray(code_size)
    struct.pack_into('<I', code, 0, 1 << 10)
    control = bytearray(96)
    struct.pack_into('<II', control, 0, 0x98761234, 0xc9)
    struct.pack_into('<I', control, 0x30, len(control))
    struct.pack_into('<I', control, 0x44, len(code))
    return struct.pack('<III12x', 0x19866891, len(code), len(control)) + code + control


def lz4_blocks(data):
    lib = ctypes.CDLL(ctypes.util.find_library('lz4'))
    lib.LZ4_createStream.restype = ctypes.c_void_p
    lib.LZ4_freeStream.argtypes = [ctypes.c_void_p]
    lib.LZ4_compress_fast_continue.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                             ctypes.c_void_p, ctypes.c_int,
                                             ctypes.c_int, ctypes.c_int]
    lib.LZ4_compress_fast_continue.restype = ctypes.c_int
    stream = lib.LZ4_createStream()
    source = ctypes.create_string_buffer(data)
    result = bytearray()
    try:
        for offset in range(0, len(data), 65536):
            size = min(65536, len(data) - offset)
            target = ctypes.create_string_buffer(66000)
            count = lib.LZ4_compress_fast_continue(stream, ctypes.byref(source, offset), target,
                                                  size, len(target), 1)
            assert count > 0
            result += struct.pack('<I', count) + target.raw[:count]
    finally:
        lib.LZ4_freeStream(stream)
    return bytes(result)


def archive(data, *, compress=False, zip64=False, crc_delta=0, flags=8,
            bad_offset=False, wrong_size=False, bad_block=False, prefix=b''):
    body = lz4_blocks(data) if compress else data
    if bad_block:
        body = struct.pack('<I', 0xffffffff) + body[4:]
    size = len(data) + (1 if wrong_size else 0)
    crc = zlib.crc32(data) ^ crc_delta
    name = b'0123456789abcdef'
    local = struct.pack('<I5H3I2H', 0x04034b50, 45, flags, 0, 0, 0, 0, 0, 0,
                        len(name), 0) + name
    offset = 0xffffffffffffffff if bad_offset and zip64 else 0xfffffffe if bad_offset else len(prefix)
    extra = struct.pack('<HH3Q', 1, 24, size, len(body), offset) if zip64 else b''
    central = struct.pack('<I6H3I5H2I', 0x02014b50, 45, 45, flags, 0, 0, 0, crc,
                          0xffffffff if zip64 else len(body),
                          0xffffffff if zip64 else size,
                          len(name), len(extra), 0, 0, 0, 0,
                          0xffffffff if zip64 else offset) + name + extra
    result = prefix + local + body
    cd_offset = len(result)
    result += central
    if zip64:
        z64_offset = len(result)
        result += struct.pack('<IQHHII4Q', 0x06064b50, 44, 45, 45, 0, 0, 1, 1,
                              len(central), cd_offset)
        result += struct.pack('<IIQI', 0x07064b50, 0, z64_offset, 1)
    result += struct.pack('<I4H2IH', 0x06054b50, 0, 0,
                          0xffff if zip64 else 1, 0xffff if zip64 else 1,
                          len(central), 0xffffffff if zip64 else cd_offset, 0)
    return result


class ExtractZip(unittest.TestCase):
    def extract(self, data):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / 'romfs').mkdir()
            (root / 'romfs' / 'data.zip').write_bytes(data)
            run = subprocess.run([str(TOOL), 'extract', '--game', str(root / 'romfs'),
                                  '--out', str(root / 'out'), '--threads', '1'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            programs = (root / 'out.programs').read_bytes()
            self.assertEqual(programs[:8], b'NXSX0001')
            return struct.unpack_from('<I', programs, 8)[0], (root / 'out.errors.txt').read_text()

    def test_stored_shader(self):
        self.assertEqual(self.extract(archive(shader())), (1, ''))

    def test_lz4_shader(self):
        self.assertEqual(self.extract(archive(shader(), compress=True)), (1, ''))

    def test_zip64_dictionary_blocks(self):
        self.assertEqual(self.extract(archive(shader(160000), compress=True, zip64=True)), (1, ''))

    def test_vzip_index_prefix(self):
        self.assertEqual(self.extract(archive(shader(160000), compress=True, zip64=True,
                                              prefix=b'VZIP' + bytes(100))), (1, ''))

    def test_nonshader_skipped(self):
        self.assertEqual(self.extract(archive(b'not a shader' * 100, compress=True)), (0, ''))

    def test_bad_crc(self):
        count, error = self.extract(archive(shader(), compress=True, crc_delta=1))
        self.assertEqual(count, 0)
        self.assertIn('CRC mismatch', error)

    def test_bad_member_offset(self):
        count, error = self.extract(archive(shader(), zip64=True, bad_offset=True))
        self.assertEqual(count, 0)
        self.assertIn('outside data region', error)

    def test_wrong_decoded_length(self):
        count, error = self.extract(archive(shader(), compress=True, wrong_size=True))
        self.assertEqual(count, 0)
        self.assertIn('wrong decoded length', error)

    def test_bad_block_length(self):
        count, error = self.extract(archive(shader(), compress=True, bad_block=True))
        self.assertEqual(count, 0)
        self.assertIn('block exceeds bounds', error)

    def test_encrypted_rejected(self):
        count, error = self.extract(archive(shader(), flags=9))
        self.assertEqual(count, 0)
        self.assertIn('unsupported ZIP member encoding', error)

    def test_unsupported_flags_rejected(self):
        count, error = self.extract(archive(shader(), flags=0x48))
        self.assertEqual(count, 0)
        self.assertIn('unsupported ZIP member encoding', error)

    def test_split_archive_rejected(self):
        data = bytearray(archive(shader()))
        struct.pack_into('<H', data, len(data) - 18, 1)
        count, error = self.extract(data)
        self.assertEqual(count, 0)
        self.assertIn('split ZIP', error)

    def test_bad_directory_size(self):
        data = bytearray(archive(shader()))
        struct.pack_into('<I', data, len(data) - 10, 0x7fffffff)
        count, error = self.extract(data)
        self.assertEqual(count, 0)
        self.assertIn('directory exceeds bounds', error)

    def test_bad_shader_lengths(self):
        data = bytearray(shader())
        struct.pack_into('<I', data, 4, 0xffffffff)
        count, error = self.extract(archive(data))
        self.assertEqual(count, 0)
        self.assertIn('sizes do not match', error)

    def test_truncated_archive(self):
        count, error = self.extract(archive(shader())[:-10])
        self.assertEqual(count, 0)
        self.assertIn('end record missing', error)


if __name__ == '__main__':
    unittest.main()
