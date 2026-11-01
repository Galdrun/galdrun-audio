"""Factory-compatible signed FIT packaging shared by Connector 3 profiles."""
from pathlib import Path
import hashlib
import os
import re
import subprocess
import tempfile


def sign_fit(kernel_path, dtb_path, diagnostic, signing, description, monitor_path=None):
    tools = diagnostic / "host/bin"

    def run(tool, *args, **kwargs):
        result = subprocess.run([str(tools / tool), *map(str, args)],
                                env=dict(os.environ, PATH=f"{tools}:{os.environ.get('PATH', '')}"),
                                capture_output=True, **kwargs)
        if result.returncode:
            raise ValueError(f"{tool} failed: {result.stdout!r} {result.stderr!r}")
        return result

    def prop(image, node, name, kind="s"):
        return run("fdtget", "-t", kind, image, node, name, text=True).stdout.strip()

    with tempfile.TemporaryDirectory(prefix="connector3-fit-") as directory:
        stage = Path(directory)
        kernel = kernel_path.read_bytes()
        if not 0 < len(kernel) <= 0x800000:
            raise ValueError("Kernel exceeds the conservative 8 MiB FIT loader limit")
        (stage / "zImage").write_bytes(kernel)
        tree = run("dtc", "-I", "dtb", "-O", "dts", dtb_path).stdout.decode()
        tree, count = re.subn(r"(?m)^([ \t]*)bus@30800000 \{", r"\1aips-bus@30800000 {", tree)
        if count != 1:
            raise ValueError("Expected exactly one factory Ethernet parent bus")
        tree = tree.replace("/soc/bus@30800000/", "/soc/aips-bus@30800000/")
        dtb = stage / "diagnostic.dtb"
        run("dtc", "-I", "dts", "-O", "dtb", "-o", dtb, input=tree.encode())
        serial = prop(dtb, "/aliases", "serial0")
        if (prop(dtb, serial, "status") != "okay" or prop(dtb, serial, "reg", "x") != "30860000 10000"
                or prop(dtb, "/chosen", "stdout-path") != "serial0:115200n8"):
            raise ValueError("Factory bus rename broke the UART1 console/serial0 alias")
        run("fdtget", dtb, "/soc/aips-bus@30800000/ethernet@30bf0000/mdio/ethernet-phy@0", "reg")
        monitor = (monitor_path or diagnostic / "images/tee-raw.bin").read_bytes()
        (stage / "tee-raw.bin").write_bytes(monitor)
        template = (diagnostic / "images/diagnostic.its").read_text()
        (stage / "firmware.its").write_text(template.replace("RAM diagnostics", description))
        control, image = stage / "control.dtb", stage / "firmware.itb"
        run("dtc", "-I", "dts", "-O", "dtb", "-o", control, input=b"/dts-v1/; / {};\n")
        run("mkimage", "-f", "firmware.its", "-k", signing, "-K", control, "-r", image, cwd=stage)
        if not 0 < image.stat().st_size <= 0xC00000:
            raise ValueError("FIT exceeds the factory 12 MiB limit")
        if prop(control, "/signature/key-verified-boot", "required") != "conf":
            raise ValueError("FIT configuration signature must be required")
        run("fit_check_sign", "-f", image, "-k", control)
        config = "/configurations/Connector"
        if prop(image, config + "/signature-1", "sign-images") != "kernel fdt loadables":
            raise ValueError("Kernel, device tree and secure monitor must all be signed")
        if prop(image, "/images/kernel-1", "entry", "x") != "80800000":
            raise ValueError("Unexpected Linux entry address")
        for name, expected in (("kernel", "kernel-1"), ("fdt", "fdt-1"), ("loadables", "tee-1")):
            if prop(image, config, name) != expected:
                raise ValueError(f"Unexpected FIT component: {name}")
        for index, name, expected, address in (
                (0, "kernel-1", kernel, "80800000"),
                (1, "fdt-1", dtb.read_bytes(), "83000000"),
                (2, "tee-1", monitor, "9fd00000")):
            extracted = stage / f"component-{index}"
            run("dumpimage", "-T", "flat_dt", "-p", index, "-o", extracted, image)
            if extracted.read_bytes() != expected or prop(image, f"/images/{name}", "load", "x") != address:
                raise ValueError(f"Signed component differs from its verified input: {name}")
            digest = bytes(int(part, 16) for part in prop(image, f"/images/{name}/hash-1", "value", "bx").split())
            if digest != hashlib.sha256(expected).digest():
                raise ValueError(f"Signed component hash mismatch: {name}")
        bad = stage / "tampered.itb"
        bad.write_bytes(image.read_bytes())
        node = config + "/signature-1"
        signature = prop(image, node, "value", "bx").split()
        signature[0] = f"{int(signature[0], 16) ^ 1:x}"
        run("fdtput", "-t", "bx", bad, node, "value", *signature)
        rejected = subprocess.run([str(tools / "fit_check_sign"), "-f", str(bad), "-k", str(control)],
                                  capture_output=True)
        if rejected.returncode == 0:
            raise ValueError("Tampered FIT signature was accepted")
        payload = image.read_bytes()
        return payload, {"fit_bytes": len(payload), "fit_sha256": hashlib.sha256(payload).hexdigest(),
                         "signed_dtb_sha256": hashlib.sha256(dtb.read_bytes()).hexdigest(),
                         "monitor_sha256": hashlib.sha256(monitor).hexdigest(),
                         "signature_verified": True, "tampered_signature_rejected": True,
                         "uart_alias_verified": True, "monitor_production_qualified": False}
