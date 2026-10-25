"""Regression for final server image provenance instead of client image metadata."""
from pathlib import Path
import hashlib
import json
import runpy
import struct
import tempfile

scripts = Path(__file__).resolve().parent
write_manifest = runpy.run_path(str(scripts / 'server-image-manifest.py'))['write_manifest']
with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    images, server, core = [root / name for name in ('images', 'server', 'core')]
    for directory in (images, server, core):
        directory.mkdir()
    mbr = bytearray(512)
    for index, kind, start, sectors in ((0, 12, 2048, 131072), (1, 131, 133120, 6291456)):
        struct.pack_into('<B3sB3sII', mbr, 446 + 16 * index, 0, bytes(3), kind, bytes(3), start, sectors)
    (images / 'sdcard.img').write_bytes(mbr)
    (images / 'boot.vfat').write_bytes(b'unchanged boot')
    (images / 'rootfs.ext4').write_bytes(b'server filesystem')
    (server / 'galdrun-server').write_bytes(b'new server')
    (core / 'galdrun-core').write_bytes(b'new core')
    source = root / 'client.json'
    source.write_text(json.dumps(dict(client={'sha256': 'client hash'}, supportedModels=['Pi 2'],
        artifacts={'sdcard.img': {'sha256': 'old image'}, 'linux.config': {'sha256': 'kernel config'}})))
    output = root / 'server.json'
    write_manifest(source, images, server, core, 'linux-armv7', 'rpi3', output)
    manifest = json.loads(output.read_text())
    assert manifest['client']['sha256'] == 'client hash'
    assert manifest['artifacts']['linux.config']['sha256'] == 'kernel config'
    assert manifest['artifacts']['rootfs.ext4']['sha256'] == hashlib.sha256(b'server filesystem').hexdigest()
    assert manifest['core']['sha256'] == hashlib.sha256(b'new core').hexdigest()
    assert manifest['partitions'][1]['sectors'] * 512 == 3 * 1024**3
    assert manifest['imageType'] == 'server'
    assert manifest['supportedModels'] == ['Raspberry Pi 3 Model B', 'Raspberry Pi 3 Model B+']
print('PASS: server image metadata describes final artifacts and supported models')
