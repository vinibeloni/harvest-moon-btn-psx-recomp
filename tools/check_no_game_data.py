#!/usr/bin/env python3
"""Refuse to let copyrighted game data into this repository.

This repo ships code and addresses, never the game: no disc image, no EXE, no
ROM, no retail BIOS, no save, no code generated from the game's executable, and
no dump of its RAM. The user supplies the disc.

    python3 tools/check_no_game_data.py            # every tracked file
    python3 tools/check_no_game_data.py --staged   # only what is about to be committed

Exit status is 1 when anything is found. Enable it on every commit with:

    git config core.hooksPath .githooks

The submodule psxrecomp/ is a separate repository and is not inspected here.
"""
import re
import subprocess
import sys

# Extensions that are never source code here.
BAD_EXT = {
    '.bin', '.cue', '.iso', '.img', '.chd', '.mcd', '.mcr', '.pst', '.sav',
    '.exe', '.dll', '.so', '.dylib', '.elf', '.rom', '.z64', '.n64', '.v64',
    '.psxmod',
}
# Names that identify the game's own files or a retail BIOS.
BAD_NAME = re.compile(r'(^|/)(SLUS|SLES|SLPS|SCUS|SCPH)[_-]?[0-9A-Z.]*$|A_FILE\.|\.text\.bin$',
                      re.IGNORECASE)
# Directories whose contents are derived from the game's executable or discs.
BAD_DIR = ('generated/', 'cache/', 'build/', 'captures/', 'dist/')
BAD_BASENAME = ('overlay_captures.json', 'overlay_captures.addendum.jsonl',
                'overlay_static.json', 'overlays_static.c')

MAX_BYTES = 1 << 20          # nothing legitimate here is over 1 MiB
HEX_RUN = re.compile(r'(?:[0-9a-fA-F]{2}){64,}')       # 64+ bytes written as hex
B64_RUN = re.compile(r'[A-Za-z0-9+/=]{300,}')           # a long base64 blob


def git(*args):
    return subprocess.run(['git', *args], capture_output=True, text=True,
                          check=True).stdout


def files(staged):
    if staged:
        out = git('diff', '--cached', '--name-only', '--diff-filter=ACM', '-z')
    else:
        out = git('ls-files', '-z')
    return [f for f in out.split('\0') if f and f != 'psxrecomp']


def read_staged(path):
    return subprocess.run(['git', 'show', ':' + path], capture_output=True).stdout


def problems(path, staged):
    found = []
    low = path.lower()
    base = low.rsplit('/', 1)[-1]
    ext = '.' + base.rsplit('.', 1)[-1] if '.' in base else ''
    if ext in BAD_EXT:
        found.append('binary/disc/save extension %s' % ext)
    if BAD_NAME.search(path):
        found.append('looks like a game or BIOS file name')
    if low.startswith(BAD_DIR):
        found.append('lives in a directory of generated/derived data')
    if base in BAD_BASENAME:
        found.append('derived from the game executable or disc')

    data = read_staged(path) if staged else None
    if data is None:
        try:
            with open(path, 'rb') as f:
                data = f.read()
        except OSError:
            return found
    if len(data) > MAX_BYTES:
        found.append('%.1f MiB: larger than any legitimate file here' % (len(data) / 1048576))
    if b'\0' in data[:8192]:
        found.append('binary content (NUL bytes)')
    else:
        text = data.decode('utf-8', errors='replace')
        if HEX_RUN.search(text):
            found.append('long run of hex bytes (a data dump?)')
        if B64_RUN.search(text):
            found.append('long base64 blob (embedded binary?)')
    return found


def main(argv):
    staged = '--staged' in argv
    bad = []
    for f in files(staged):
        p = problems(f, staged)
        if p:
            bad.append((f, p))
    if not bad:
        print('check_no_game_data: OK (%s)' % ('staged files' if staged else 'all tracked files'))
        return 0
    print('check_no_game_data: REFUSED. These must not be committed:', file=sys.stderr)
    for f, p in bad:
        print('  %s' % f, file=sys.stderr)
        for reason in p:
            print('      - %s' % reason, file=sys.stderr)
    print('\nThe repo ships code and addresses only. See CLAUDE.md, "No game data".',
          file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
