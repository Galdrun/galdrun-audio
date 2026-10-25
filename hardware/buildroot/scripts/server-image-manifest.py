"""Describe the assembled server image, retaining its client's boot provenance."""
from pathlib import Path
import hashlib
import json
import struct
import sys


def artifact(path):
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(bytes=path.stat().st_size, sha256=digest)


def write_manifest(source, images, server, core, arch, board, output):
    manifest = json.loads(source.read_text())
    manifest['imageType'] = 'server'
    manifest['supportedModels'] = (['Raspberry Pi 3 Model B', 'Raspberry Pi 3 Model B+']
        if board == 'rpi3' else [f'Raspberry Pi {board[-1]} Model B'])
    manifest['server'] = dict(target=arch, **artifact(server / 'galdrun-server'))
    manifest['core'] = dict(target=arch, **artifact(core / 'galdrun-core'))
    manifest['validation'] = 'host image structure and artifact hashes; hardware qualification pending'
    # Kernel/config provenance still belongs to the unchanged client boot image.
    for name in ('sdcard.img', 'boot.vfat', 'rootfs.ext4'):
        manifest['artifacts'][name] = artifact(images / name)
    mbr = (images / 'sdcard.img').open('rb')
    with mbr:
        sector = mbr.read(512)
    manifest['partitions'] = []
    for index in range(2):
        kind = sector[446 + 16 * index + 4]
        start, sectors = struct.unpack_from('<II', sector, 446 + 16 * index + 8)
        manifest['partitions'].append(dict(type=kind, startSector=start, sectors=sectors,
                                           end=(start + sectors) * 512))
    output.write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    source, images, server, core = map(Path, sys.argv[1:5])
    write_manifest(source, images, server, core, sys.argv[5], sys.argv[6], Path(sys.argv[7]))
