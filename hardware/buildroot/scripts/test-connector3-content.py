"""Run with python3 hardware/buildroot/scripts/test-connector3-content.py."""
from pathlib import Path
import hashlib
import stat
import tempfile
from unittest.mock import patch
from connector3_content import audit_files


with tempfile.TemporaryDirectory(prefix="connector3-content-test-") as directory:
    manifest = Path(directory) / "manifest.csv"
    manifest.write_text('PACKAGE,VERSION,LICENSE,SOURCE SITE\n'
                        'busybox,1.37.0,GPL-2.0,https://busybox.net/downloads\n')
    mode = stat.S_IFREG | 0o755
    ours = {"usr/sbin/raumfeld-network": (mode, b"#!/bin/sh\n# Galdrun network service\n")}
    assert audit_files(ours, manifest)["known_vendor_payload_checks_passed"]
    # Exercise approval using synthetic bytes; no private firmware test input.
    radio = b"test hardware microcode"
    notices = Path(__file__).resolve().parents[1] / "board/connector3/firmware"
    approved = {**ours, "lib/firmware/mrvl/pcie8897_uapsta.bin": (mode, radio)}
    for notice in ("LICENCE.Marvell", "source.json"):
        approved[f"usr/share/licenses/connector3-radio/{notice}"] = (mode, (notices / notice).read_bytes())
    with patch("connector3_content.DEVICE_RADIO_SHA256", hashlib.sha256(radio).hexdigest()):
        assert audit_files(approved, manifest)["approved_hardware_radio_present"]
        for notice in ("LICENCE.Marvell", "source.json"):
            key = f"usr/share/licenses/connector3-radio/{notice}"
            for invalid in ({k: v for k, v in approved.items() if k != key},
                            {**approved, key: (mode, b"wrong notice")}):
                try:
                    audit_files(invalid, manifest)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f"Missing/changed firmware notice accepted: {notice}")
        try:
            audit_files({**approved, "usr/sbin/hardwared": (mode, b"vendor")}, manifest)
        except ValueError:
            pass
        else:
            raise AssertionError("Approved firmware bypassed vendor runtime checks")
        try:
            audit_files({**approved, "opt/radio-copy.bin": (mode, radio)}, manifest)
        except ValueError:
            pass
        else:
            raise AssertionError("Approved firmware accepted at an unexpected path")
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
