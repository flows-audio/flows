#!/usr/bin/env python3
"""Package the browser build as a deployable static site.

Copies ui/dist-web into dist/Flows-<version>-web/ with the license texts,
third-party notices and the matching source location, then zips it. Upload the
folder's contents to any static host that sends the headers in _headers
(Cloudflare Pages and Netlify read that file directly).

Usage: package_web.py [--tag v1.2.3]
"""
from __future__ import annotations

import argparse
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = 'https://github.com/flows-audio/flows'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', help='Release tag; defaults to v<VERSION>')
    args = parser.parse_args()
    version = (ROOT / 'VERSION').read_text().strip()
    tag = args.tag or f'v{version}'
    if tag != f'v{version}':
        raise SystemExit(f'Tag {tag} does not match VERSION {version}')
    site = ROOT / 'ui' / 'dist-web'
    for required in ('index.html', '_headers', 'core/flows-core.mjs', 'core/flows-core.wasm'):
        if not (site / required).is_file():
            raise SystemExit(f'Missing {site / required}. Run: npm run build:web --prefix ui')
    emscripten = ROOT / 'licenses' / 'third-party' / 'emscripten'
    if not any(emscripten.glob('*.txt')):
        raise SystemExit('Missing Emscripten notices. Run tools/collect_licenses.py --emscripten-dir <emscripten>')
    name = f'Flows-{version}-web'
    output = ROOT / 'dist' / name
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(site, output)
    for document in ('LICENSE', 'NOTICE', 'THIRD_PARTY.md'):
        shutil.copyfile(ROOT / document, output / document)
    # Only what the browser build contains: Flows' own Apache-2.0 directories,
    # the UI runtime packages and Emscripten's runtime/system libraries (no JUCE).
    licenses = output / 'licenses'
    (licenses / 'third-party').mkdir(parents=True)
    shutil.copyfile(ROOT / 'licenses' / 'Apache-2.0.txt', licenses / 'Apache-2.0.txt')
    shutil.copyfile(ROOT / 'licenses' / 'third-party' / 'ui.txt', licenses / 'third-party' / 'ui.txt')
    shutil.copytree(emscripten, licenses / 'third-party' / 'emscripten')
    (output / 'SOURCE.txt').write_text(
        f'Flows {version} is free software under AGPL-3.0-only (see LICENSE).\n'
        f'Corresponding source: {REPOSITORY}/tree/{tag}\n'
        f'Release archives: {REPOSITORY}/releases/tag/{tag}\n')
    archive = ROOT / 'dist' / f'{name}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(output.rglob('*')):
            if path.is_file():
                bundle.write(path, Path(name) / path.relative_to(output))
    print(archive)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
