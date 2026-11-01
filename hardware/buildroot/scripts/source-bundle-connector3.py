"""Collect corresponding package sources, local build inputs and licence records."""
from pathlib import Path
import csv
import hashlib
import importlib.util
import io
import json
import stat
import subprocess
import sys
import tarfile
import urllib.request


project = Path(__file__).resolve().parents[1]
repo = project.parent.parent


def gcc_source(version):
    folder = project / 'downloads/toolchain-sources'
    folder.mkdir(parents=True, exist_ok=True)
    name = f'gcc-{version}.tar.xz'
    base = f'https://gcc.gnu.org/pub/gcc/releases/gcc-{version}/'
    sums = folder / f'gcc-{version}.sha512.sum'
    if not sums.exists():
        with urllib.request.urlopen(base + 'sha512.sum', timeout=60) as response:
            sums.write_bytes(response.read())
    expected = next(line.split()[0] for line in sums.read_text().splitlines() if line.split()[-1] == name)
    archive = folder / name
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        with urllib.request.urlopen(base + name, timeout=60) as response, partial.open('wb') as stream:
            while chunk := response.read(1 << 20): stream.write(chunk)
        partial.rename(archive)
    if hashlib.sha512(archive.read_bytes()).hexdigest() != expected:
        raise ValueError('GCC source does not match the upstream checksum: ' + name)
    return archive, sums


def write(work, destination):
    spec = importlib.util.spec_from_file_location('source_bundle', project / 'scripts/source-bundle.py')
    bundle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bundle)
    trees = ['connector3-ram', 'connector3-installer-check']
    if (work/'connector3-installer-write/legal-info/manifest.csv').is_file():
        trees.append('connector3-installer-write')
    sources = dict(bundle.members(work, trees))
    for version in ('13.3.0', '14.3.0'):
        archive, sums = gcc_source(version)
        sources['toolchain/' + archive.name] = archive
        sources['toolchain/' + sums.name] = sums
    # This is the actual ALSA header/source input used by the exported client.
    alsa = Path.home() / '.local/share/multiroom-linux/downloads/alsa-lib-1.2.4.tar.bz2'
    sources['client-inputs/' + alsa.name] = alsa
    for tree in (*trees, 'connector3-diagnostic'):
        info = work / tree / 'legal-info'
        for name in ('manifest.csv', 'README', 'legal-info.sha256', 'buildroot.config'):
            sources[f'legal-info/{tree}/{name}'] = info / name
        for path in (info / 'licenses').rglob('*'):
            if path.is_file(): sources[f'legal-info/{tree}/licenses/{path.relative_to(info / "licenses")}'] = path
        with (info/'manifest.csv').open(newline='') as stream:
            for row in csv.DictReader(stream):
                if 'GPL' in row['LICENSE'] or row['PACKAGE'] in ('toolchain-external-bootlin','linux-firmware'):
                    continue
                folder = info/'sources'/f"{row['PACKAGE']}-{row['VERSION']}"
                for path in folder.rglob('*'):
                    if path.is_file(): sources[f'legal-info/{tree}/sources/{path.relative_to(info/"sources")}'] = path
    if not any('optee-os' in name and name.endswith(('.gz','.xz','.bz2')) for name in sources):
        raise ValueError('Source-built monitor archive missing')
    prefixes = ('client', 'shared', 'hardware/buildroot/board/connector3', 'hardware/buildroot/board/common',
                'hardware/buildroot/board/connector2/status-leds.cpp',
                'hardware/buildroot/scripts', 'hardware/buildroot/PrepareHost.sh', 'hardware/buildroot/external.mk',
                'hardware/buildroot/external.desc', 'hardware/buildroot/Config.in', 'scripts/build-client.sh',
                'scripts/setup/linux.sh')
    names = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', *prefixes], cwd=repo)
    for raw in names.split(b'\0'):
        if not raw: continue
        name = raw.decode()
        path = repo / name
        if any(part in ('private', 'reference', 'downloads', 'output', '.git', '__pycache__') for part in path.relative_to(repo).parts):
            raise ValueError('Private/generated path in source snapshot: ' + name)
        if path.is_file() and path.resolve().is_relative_to(repo.resolve()):
            sources['project-source/' + name] = path
    summary = work / 'connector3-ram/host/opt/ext-toolchain/summary.csv'
    sources['toolchain/bootlin-summary.csv'] = summary
    sources['toolchain/busybox-persistent.config'] = work / 'connector3-persistent-busybox/.config'
    compiler = Path.home()/'.local/share/multiroom-linux/x-tools/armv6-rpi-linux-gnueabihf'
    for name in ('build.log.bz2','.config','config'):
        if (compiler/name).is_file(): sources['toolchain/client-'+name.lstrip('.')] = compiler/name
    manifest = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in sorted(sources.items())}
    tag = 'connector3-sources-' + hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()[:12]
    destination.mkdir(parents=True, exist_ok=True)
    target = destination / (tag + '.tar')
    with tarfile.open(target, 'w', format=tarfile.PAX_FORMAT) as archive:
        text = (f'{tag}\n\nBuildroot package archives/patches and manifests, Bootlin glibc/GCC 13.3.0, '
                'GCC 14.3.0 runtime sources for the exported client, OP-TEE source and a working-tree snapshot of local build inputs.\n'
                'project-source/ contains the actual uncommitted source/config snapshot; it is not a claim that HEAD contains these changes.\n'
                'No local private signing/SSH keys, device settings or original vendor rootfs backups are included.\n'
                'The approved Marvell radio notice/source record is under project-source/hardware/buildroot/board/connector3/firmware/.\n').encode()
        for name, data in (('README.txt', text), ('SHA256.json', (json.dumps(manifest, indent=2) + '\n').encode())):
            info = tarfile.TarInfo(tag + '/' + name); info.size = len(data); info.mode = 0o644
            archive.addfile(info, io.BytesIO(data))
        for name, path in sorted(sources.items()):
            info = tarfile.TarInfo(tag + '/' + name)
            info.size = path.stat().st_size
            info.mode = stat.S_IMODE(path.stat().st_mode)
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = 'root'
            with path.open('rb') as stream: archive.addfile(info, stream)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    (destination / (target.name + '.sha256')).write_text(digest + '  ' + target.name + '\n')
    report = {'source_archive': target.name, 'sha256': digest, 'files': len(sources),
              'private_device_inputs_included': False, 'local_source_snapshot_included': True}
    (destination / 'source-bundle-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    write(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
