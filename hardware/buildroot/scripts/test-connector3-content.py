"""Run with python3 hardware/buildroot/scripts/test-connector3-content.py."""
from pathlib import Path
import stat
import tempfile
from connector3_content import audit_files


with tempfile.TemporaryDirectory(prefix="connector3-content-test-") as directory:
    manifest = Path(directory) / "manifest.csv"
    manifest.write_text('PACKAGE,VERSION,LICENSE,SOURCE SITE\n'
                        'busybox,1.37.0,GPL-2.0,https://busybox.net/downloads\n')
    mode = stat.S_IFREG | 0o755
    ours = {"usr/sbin/raumfeld-network": (mode, b"#!/bin/sh\n# Galdrun network service\n")}
    assert audit_files(ours, manifest)["known_vendor_payload_checks_passed"]
    for forbidden in ("usr/lib/libraumfeldcpp.so.0", "usr/sbin/hardwared",
                      "etc/raumfeld/config", "usr/sbin/galdrun-connector3-buttons"):
        try:
            audit_files({**ours, forbidden: (mode, b"not a Galdrun payload")}, manifest)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Vendor payload accepted: {forbidden}")
    manifest.write_text('PACKAGE,VERSION,LICENSE,SOURCE SITE\n'
                        'something,1,unknown,https://updates.raumfeld.com/software\n')
    try:
        audit_files(ours, manifest)
    except ValueError:
        pass
    else:
        raise AssertionError("Vendor source accepted")
print("Connector 3 content guards: PASS")
