#!/usr/bin/env python3
"""Build the standalone Hermes T-Embed firmware with its pinned ESP-IDF."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--matrix', action='store_true')
    parser.add_argument('--profile', default='t-embed', choices=['t-embed'])
    parser.add_argument('--version', default='local')
    parser.add_argument('--output', type=Path, default=ROOT / 'artifacts')
    parser.add_argument('--build-root', type=Path, default=ROOT)
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    catalog = json.loads((ROOT / 'firmware-targets.json').read_text())
    if args.matrix:
        print(json.dumps({'include': catalog['profiles']}))
        return
    profile = catalog['profiles'][0]
    sdk = os.environ.get('IDF_PATH')
    if not sdk:
        parser.error('Activate the pinned ESP-IDF environment first')
    revision = subprocess.check_output(['git', '-C', sdk, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != profile['idf_ref']:
        parser.error(f"Requires ESP-IDF {profile['idf_ref']}; found {revision}")
    output = args.output.resolve() / args.profile
    if output.exists():
        parser.error(f'Output already exists: {output}; choose a fresh artifact directory')
    build = args.build_root.resolve() / ('build-' + args.profile)
    env = dict(os.environ, IDF_COMPONENT_MANAGER='1', IDF_PY_BUILD_JOBS=str(args.jobs))
    subprocess.run([sys.executable, str(Path(sdk) / 'tools/idf.py'),
                    '-C', str(ROOT), '-B', str(build), '-DIDF_TARGET=esp32s3',
                    '-DSDKCONFIG=' + str(build / 'sdkconfig'),
                    '-DSDKCONFIG_DEFAULTS=' + str(ROOT / 'sdkconfig.defaults'),
                    'build'], env=env, check=True)
    flash = json.loads((build / 'flasher_args.json').read_text())
    output.mkdir(parents=True)
    parts = []
    for address, name in sorted(flash['flash_files'].items(), key=lambda item: int(item[0], 0)):
        source = (build / name).resolve()
        if not source.is_relative_to(build):
            raise ValueError('Flash image must be inside the build directory')
        target = output / source.name
        shutil.copyfile(source, target)
        parts.append({'name': target.name, 'offset': int(address, 0),
                      'sha256': hashlib.sha256(target.read_bytes()).hexdigest()})
    manifest = {'profile': args.profile, 'version': args.version,
                'idf_commit': revision, 'flash_settings': flash['flash_settings'], 'parts': parts}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
