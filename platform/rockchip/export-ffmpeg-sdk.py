#!/usr/bin/env python3
"""Run on a trusted RK3399 board to export its FFmpeg SDK for cross linking."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", help="New .tar.gz archive; existing files are not overwritten")
args = parser.parse_args()
output = Path(args.output)
if output.exists():
    parser.error("Output already exists")
roots = [Path('/usr/lib/aarch64-linux-gnu/lib' + n + '.so') for n in ['avformat', 'avcodec', 'avutil', 'avdevice', 'avfilter', 'swscale', 'swresample']]
paths = set(roots)
for root in roots:
    result = subprocess.check_output(['ldd', str(root)], text=True)
    if 'not found' in result:
        raise RuntimeError('Missing board dependency: ' + result)
    for line in result.splitlines():
        fields = line.split()
        if '=>' in fields and len(fields) > 2 and fields[2].startswith('/'):
            paths.add(Path(fields[2]))
manifest = {'uname': subprocess.check_output(['uname', '-a'], text=True).strip(), 'libraries': []}
# Exclusive creation keeps a previous SDK export intact.
with output.open('xb') as destination:
    with tarfile.open(fileobj=destination, mode='w:gz', dereference=True) as archive:
        for path in sorted(paths):
            folder = 'lib' if re.match(r'lib(av(format|codec|util|device|filter)|sw(scale|resample))\.so', path.name) else 'link-deps'
            archive.add(path, arcname=folder + '/' + path.name)
            manifest['libraries'].append({'source': str(path), 'resolved': str(path.resolve()),
                'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        for name in ['libavformat', 'libavcodec', 'libavutil', 'libavdevice', 'libavfilter', 'libswscale', 'libswresample']:
            archive.add('/usr/include/aarch64-linux-gnu/' + name, arcname='include/' + name)
        import io
        data = (json.dumps(manifest, indent=2) + '\n').encode()
        info = tarfile.TarInfo('manifest.json')
        info.size = len(data)
        archive.addfile(info, io.BytesIO(data))
print(output, 'libraries:', len(paths))
