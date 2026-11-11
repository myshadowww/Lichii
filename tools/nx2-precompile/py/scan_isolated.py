#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run shader scans in bounded subprocesses, preserving every failed program in the TSV.

Writes the usual PREFIX.tsv/PREFIX.envs plus PREFIX.scan.json and per-batch logs.
Requires POSIX address-space limits. No third-party Python packages are needed.
"""
import argparse
import collections
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import json
import mmap
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import threading

STAGES = ('VertexB', 'TessellationControl', 'TessellationEval', 'Geometry', 'Fragment',
          'Compute', 'Task', 'Mesh')
HEADER = ('file\tindex\tstage\tstatus\tcode_hash\tnative_key\tcode_words\tenv_bytes\tqueries\t'
          'generic_loads\theader_reads\terror\n')


@dataclass(frozen=True)
class Program:
    begin: int
    end: int
    stage: int
    fingerprint: int


def read_programs(data):
    if len(data) < 12 or data[:8] != b'NXSX0001':
        raise ValueError('not an extracted NXSX0001 program file')
    count = struct.unpack_from('<I', data, 8)[0]
    if count > (len(data) - 12) // 21:
        raise ValueError('program count exceeds the input size')
    programs = []
    offset = 12
    for _ in range(count):
        if len(data) - offset < 21:
            raise ValueError('truncated program header')
        stage, fingerprint, control, code, origin = struct.unpack_from('<BQIII', data, offset)
        end = offset + 21 + origin + control + code
        if stage >= len(STAGES) or end > len(data):
            raise ValueError('invalid program stage or length')
        programs.append(Program(offset, end, stage, fingerprint))
        offset = end
    if offset != len(data):
        raise ValueError('trailing bytes in extracted program file')
    return programs


def remap_outputs(prefix, indices):
    rows = prefix.with_suffix('.tsv').read_text().splitlines()
    if not rows or rows[0] + '\n' != HEADER or len(rows) != len(indices) + 1:
        raise ValueError('invalid scan TSV header or row count')
    mapped = []
    fields_by_ordinal = []
    for expected, row in enumerate(rows[1:]):
        fields = row.split('\t')
        if len(fields) != 12 or fields[0] != 'programs' or int(fields[1]) != expected:
            raise ValueError('invalid scan TSV ordinal')
        fields[1] = str(indices[expected])
        fields_by_ordinal.append(fields)
        mapped.append('\t'.join(fields) + '\n')
    data = prefix.with_suffix('.envs').read_bytes()
    if data[:8] != b'NXPENV01':
        raise ValueError('invalid scan environment header')
    pos = 8
    seen = set()
    envs = []
    while pos < len(data):
        if len(data) - pos < 16:
            raise ValueError('truncated environment header')
        ordinal, key, size = struct.unpack_from('<IQI', data, pos)
        pos += 16
        if ordinal >= len(indices) or ordinal in seen or size > len(data) - pos:
            raise ValueError('invalid environment ordinal or length')
        fields = fields_by_ordinal[ordinal]
        if fields[3] != 'ok' or key != int(fields[4], 16) or size != int(fields[7]):
            raise ValueError('environment disagrees with its scan TSV status, hash or size')
        seen.add(ordinal)
        envs.append(struct.pack('<IQI', indices[ordinal], key, size) + data[pos:pos + size])
        pos += size
    if any((fields[3] == 'ok') != (ordinal in seen)
           for ordinal, fields in enumerate(fields_by_ordinal)):
        raise ValueError('successful scan TSV row has no environment')
    return mapped, envs


class Scanner:
    def __init__(self, args, data, programs):
        self.args = args
        self.data = data
        self.programs = programs
        self.batches = Path(str(args.out) + '.scan-batches')
        self.batches.mkdir(parents=True, exist_ok=True)
        self.attempts = []
        self.lock = threading.Lock()

    def scan(self, indices):
        prefix = self.batches / f'{indices[0]:08d}-{indices[-1]:08d}'
        package = prefix.with_suffix('.programs')
        with package.open('wb') as out:
            out.write(b'NXSX0001' + struct.pack('<I', len(indices)))
            for i in indices:
                p = self.programs[i]
                out.write(self.data[p.begin:p.end])
        command = [sys.executable, str(Path(__file__).resolve()), '--_exec-limit',
                   str(self.args.memory_mib * 1024 * 1024), str(self.args.tool),
                   'scan', '--pkg', str(package), '--out', str(prefix), '--threads', '1']
        if self.args.priors:
            command.extend(('--priors', str(self.args.priors)))
        status = None
        with prefix.with_suffix('.log').open('w') as log:
            try:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=self.args.timeout)
                status = result.returncode
            except subprocess.TimeoutExpired:
                status = 'timeout'
            if status == 0:
                try:
                    output = remap_outputs(prefix, indices)
                except (OSError, ValueError, struct.error) as error:
                    status = 'invalid-output'
                    log.write(str(error) + '\n')
        with self.lock:
            self.attempts.append(dict(first=indices[0], count=len(indices), status=status))
        if status == 0:
            package.unlink()
            return output
        if len(indices) > 1:
            middle = len(indices) // 2
            left = self.scan(indices[:middle])
            right = self.scan(indices[middle:])
            package.unlink()
            return left[0] + right[0], left[1] + right[1]
        i = indices[0]
        detail = (f'isolated scan exit {status}; {self.args.memory_mib} MiB address-space / '
                  f'{self.args.timeout:g} s limit; see {prefix.name}.log')
        row = (f'programs\t{i}\t{STAGES[self.programs[i].stage]}\tisolated-scan-failed\t'
               f'0000000000000000\t0000000000000000\t0\t0\t0\t00000000\t00000000\t{detail}\n')
        return [row], []


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--_exec-limit':
        import resource
        limit = int(sys.argv[2])
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        os.execv(sys.argv[3], sys.argv[3:])
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tool', type=Path, required=True)
    ap.add_argument('--pkg', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--priors', type=Path)
    ap.add_argument('--workers', type=int, default=2)
    ap.add_argument('--batch-size', type=int, default=256)
    ap.add_argument('--memory-mib', type=int, default=1536)
    ap.add_argument('--timeout', type=float, default=20)
    args = ap.parse_args()
    if os.name != 'posix':
        ap.error('isolated scans require POSIX address-space limits')
    if min(args.workers, args.batch_size, args.memory_mib, args.timeout) <= 0:
        ap.error('worker, batch, memory and timeout limits must be positive')
    found_tool = args.tool if args.tool.is_file() else shutil.which(str(args.tool))
    if found_tool is None:
        ap.error(f'scanner tool not found: {args.tool}')
    args.tool = Path(found_tool).resolve()
    args.out = args.out.resolve()
    if args.priors:
        args.priors = args.priors.resolve()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.pkg.open('rb') as source:
        if os.fstat(source.fileno()).st_size == 0:
            ap.error('empty program file')
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            programs = read_programs(data)
            scanner = Scanner(args, data, programs)
            groups = [list(range(start, min(start + args.batch_size, len(programs))))
                      for start in range(0, len(programs), args.batch_size)]
            rows = []
            with Path(str(args.out) + '.envs').open('wb') as envs:
                envs.write(b'NXPENV01')
                with ThreadPoolExecutor(max_workers=args.workers) as pool:
                    for scanned, environments in pool.map(scanner.scan, groups):
                        rows.extend(scanned)
                        envs.writelines(environments)
            if len(rows) != len(programs):
                raise ValueError('merged scan does not cover every program')
            Path(str(args.out) + '.tsv').write_text(HEADER + ''.join(rows))
            counts = collections.Counter(row.split('\t')[3] for row in rows)
            failed = []
            for row in rows:
                fields = row.rstrip('\n').split('\t')
                if fields[3] not in ('ok', 'mesh-hash-only'):
                    ordinal = int(fields[1])
                    failed.append(dict(ordinal=ordinal, stage=fields[2], status=fields[3],
                                       error=fields[11],
                                       fingerprint=f'{programs[ordinal].fingerprint:016x}'))
            report = dict(programs=len(programs), memory_mib=args.memory_mib,
                          timeout_seconds=args.timeout, batch_size=args.batch_size,
                          workers=args.workers, status_counts=dict(counts), failed=failed,
                          attempts=sorted(scanner.attempts, key=lambda x: (x['first'], -x['count'])))
            Path(str(args.out) + '.scan.json').write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps({k: report[k] for k in ('programs', 'status_counts')}, indent=2))


if __name__ == '__main__':
    main()
