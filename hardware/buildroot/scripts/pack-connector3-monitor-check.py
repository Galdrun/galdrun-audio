"""Test only the new monitor, retaining the qualified read-only Linux payload."""
from pathlib import Path
import hashlib
import json
import sys
from connector3_fit import sign_fit


project = Path(__file__).resolve().parents[1]
diagnostic, signing = (Path(value).resolve() for value in sys.argv[1:])
storage = project / 'output/connector3-storage-check'
monitor = project / 'output/connector3-monitor'
qualified = json.loads((storage / 'hardware-verification.json').read_text())
assert qualified['cold_boot_verified'] and qualified['all_partitions_read_only']
for name, key in (('zImage', 'kernel_sha256'), ('connector3.dtb', 'dtb_sha256')):
    assert hashlib.sha256((storage / name).read_bytes()).hexdigest() == qualified[key]
monitor_report = json.loads((monitor / 'monitor-verification.json').read_text())
assert hashlib.sha256((monitor / 'tee-raw.bin').read_bytes()).hexdigest() == monitor_report['sha256']
image, report = sign_fit(storage / 'zImage', storage / 'connector3.dtb', diagnostic,
                         signing, 'hardware RNG read-only check', monitor / 'tee-raw.bin')
destination = project / 'output/connector3-monitor-check'
destination.mkdir(parents=True, exist_ok=True)
image += bytes(0xC00000 - len(image))
(destination / 'connect3.img').write_bytes(image)
report.update({'purpose': 'read-only monitor USB check; no installer or NAND writes',
               'all_partitions_read_only': True, 'hardware_verified': False,
               'kernel_sha256': qualified['kernel_sha256'], 'source_monitor': monitor_report,
               'usb_sha256': hashlib.sha256(image).hexdigest()})
(destination / 'monitor-check-verification.json').write_text(json.dumps(report, indent=2) + '\n')
(destination / 'connect3.img.sha256').write_text(report['usb_sha256'] + '  connect3.img\n')
print(json.dumps(report, indent=2))
