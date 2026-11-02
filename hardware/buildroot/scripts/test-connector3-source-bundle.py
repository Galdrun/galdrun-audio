"""Verify the final source archive manifest and safe, public source paths."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import sys
import tarfile


folder = Path(sys.argv[1])
report = json.loads((folder/'source-bundle-verification.json').read_text())
path = folder/report['source_archive']
assert hashlib.sha256(path.read_bytes()).hexdigest() == report['sha256']
with tarfile.open(path) as archive:
    members = archive.getmembers()
    manifest_entry = next(member for member in members if member.name.endswith('/SHA256.json'))
    prefix = manifest_entry.name.removesuffix('SHA256.json')
    manifest = json.load(archive.extractfile(manifest_entry))
    for member in members:
        name = PurePosixPath(member.name)
        assert not name.is_absolute() and '..' not in name.parts
        assert not any(part in ('private','reference','.git','__pycache__') for part in name.parts)
        assert member.isfile(), member.name
    for name, expected in manifest.items():
        stream = archive.extractfile(prefix+name)
        digest = hashlib.sha256()
        while chunk := stream.read(1 << 20): digest.update(chunk)
        assert digest.hexdigest() == expected, name
    assert any('optee-os' in name for name in manifest)
    assert 'toolchain/gcc-13.3.0.tar.xz' in manifest and 'toolchain/gcc-14.3.0.tar.xz' in manifest
    assert 'project-source/hardware/buildroot/board/connector2/status-leds.cpp' in manifest, 'Shared Connector 3 LED monitor source missing'
    prefix = 'project-source/hardware/buildroot/board/connector3/'
    assert prefix+'raumfeld-suspend' in manifest, 'Connector 3 sleep helper source missing'
    assert prefix+'ram/patches/linux/0013-mwifiex-drain-work-before-unregister.patch' in manifest, 'WLAN recovery lifetime fix missing'
    assert prefix+'ram/patches/linux/0009-connector3-retain-pcie-in-suspend.patch' not in manifest, 'Retired PCIe bypass still packaged'
print('Connector 3 source archive: all file hashes match; monitor/runtime/local sources present; no private paths')
