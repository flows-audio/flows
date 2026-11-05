#!/usr/bin/env python3
"""Check a publication tree without printing matched credentials or private text."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import os

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {'.git', 'node_modules', '__pycache__', '.cache', '.local', 'dist', 'dist-web', 'test-results', 'playwright-report'}
PATTERNS = {
    'credential': re.compile(r'sk-[A-Za-z0-9_-]{18,}|gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----'),
    'personal_path': re.compile(r'/(?:Users|home)/[A-Za-z0-9_.-]+/'),
    'development_date': re.compile(r'\b20\d{2}-\d{2}-\d{2}\b|\b20\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\d|3[01])\b'),
    'private_release_ref': re.compile(r'flows-adcx-20\d\d'),
}


def files(root):
    for directory, folders, names in os.walk(root, followlinks=False):
        for name in folders:
            if (Path(directory) / name).is_symlink():
                yield Path(directory) / name
        folders[:] = sorted(n for n in folders if n not in EXCLUDED and not n.startswith('build'))
        if Path(directory).relative_to(root) == Path('test'):
            folders[:] = [n for n in folders if n != 'output']
        for name in sorted(names):
            yield Path(directory) / name


def png_metadata(data):
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        return []
    chunks, offset = [], 8
    while offset + 12 <= len(data):
        size = struct.unpack('>I', data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        if kind in (b'eXIf', b'tEXt', b'zTXt', b'iTXt', b'tIME'):
            chunks.append(kind.decode())
        offset += size + 12
    return chunks


def check(root, forbidden):
    findings, checked = [], 0
    blocked = re.compile(r'(?<![a-f0-9])(?:' + '|'.join(re.escape(sha) for sha in forbidden) + r')(?![a-f0-9])') if forbidden else None
    for p in files(root):
        relative = p.relative_to(root).as_posix()
        if p.is_symlink():
            findings.append({'file': relative, 'rule': 'unexpected_symlink'})
            continue
        data = p.read_bytes(); checked += 1
        if png_metadata(data):
            findings.append({'file': relative, 'rule': 'png_metadata'})
        try:
            text = data.decode('utf-8')
        except UnicodeDecodeError:
            if p.suffix.lower() not in ('.png', '.wav'):
                findings.append({'file': relative, 'rule': 'unexpected_binary'})
            continue
        # Third-party license dates/copyright attribution must stay intact.
        legal = relative.startswith('licenses/') or p.name in {'LICENSE', 'NOTICE'}
        for number, line in enumerate(text.splitlines(), 1):
            for name, pattern in PATTERNS.items():
                if name == 'development_date' and legal:
                    continue
                if pattern.search(line):
                    findings.append({'file': relative, 'line': number, 'rule': name})
            if blocked and blocked.search(line):
                findings.append({'file': relative, 'line': number, 'rule': 'private_commit_reference'})
    version = (root / 'VERSION').read_text().strip()
    for relative in ('ui/package.json', 'ui/package-lock.json'):
        if json.loads((root / relative).read_text())['version'] != version:
            findings.append({'file': relative, 'rule': 'version_mismatch'})
    print(json.dumps({'files_checked': checked, 'findings': findings}, indent=2))
    return not findings


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--forbidden-commits', type=Path)
    parser.add_argument('--single-commit', action='store_true')
    args = parser.parse_args()
    blocked = args.forbidden_commits.read_text().splitlines() if args.forbidden_commits else []
    success = check(args.root.resolve(), blocked)
    if args.single_commit:
        commits = subprocess.check_output(['git', 'rev-list', '--all', '--count'], cwd=args.root, text=True).strip()
        if commits != '1':
            print('Publication repository must contain exactly one reachable commit.')
            success = False
    raise SystemExit(0 if success else 1)
