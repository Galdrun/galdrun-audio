#!/usr/bin/env bash
# Build a source-only, signed USB RAM diagnostic image. Never touches hardware.
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
[[ $# == 2 ]] || { echo "Usage: $0 <SSH public key> <FIT signing-key directory>" >&2; exit 2; }
export GALDRUN_SSH_PUBKEY
GALDRUN_SSH_PUBKEY=$(realpath -- "$1")
signing=$(realpath -- "$2")
if ! grep -Eq '^(ssh-|ecdsa-|sk-)' "$GALDRUN_SSH_PUBKEY" || grep -q 'PRIVATE KEY' "$GALDRUN_SSH_PUBKEY"; then
    echo 'Supply an SSH public key, never a private key.' >&2; exit 2
fi
ssh-keygen -l -f "$GALDRUN_SSH_PUBKEY" >/dev/null
[[ -r "$signing/verified-boot.key" && -r "$signing/verified-boot.crt" ]] || {
    echo 'Missing private local FIT signing material; nothing will be built.' >&2; exit 2;
}
source "$project/PrepareHost.sh"
jobs=${JOBS:-16}
[[ $jobs =~ ^[0-9]+$ && $jobs -ge 1 && $jobs -le 32 ]] || { echo 'JOBS must be 1..32' >&2; exit 2; }
exec 9>"$work/build.lock"
flock -n 9 || { echo 'Another firmware build is running.' >&2; exit 1; }
mkdir -p "$work/external" "$project/logs" "$project/output/connector3-diagnostic"
rsync -a --delete --exclude downloads --exclude logs --exclude output --exclude reference --exclude private \
    "$project/" "$work/external/"
board="$work/external/board/connector3/diagnostic"
chmod 755 "$board/init.sh" "$board/post-build.sh"
output="$work/connector3-diagnostic"
make_args=(-C "$work/buildroot-2025.02.18" "O=$output" "BR2_EXTERNAL=$work/external" "BR2_DL_DIR=$project/downloads")
make "${make_args[@]}" "BR2_DEFCONFIG=$board/defconfig" defconfig
make "${make_args[@]}" legal-info >"$project/logs/connector3-diagnostic-licenses.log" 2>&1
# OP-TEE's build variables are part of the bootloader handoff contract. Reapply
# them even when Buildroot has a cached .stamp_built from an earlier attempt.
make "${make_args[@]}" -j"$jobs" optee-os-rebuild
# This is a dedicated experimental tree; rebuild the kernel on each invocation.
if [[ -d "$output/build/linux-6.12.112" ]]; then
    make "${make_args[@]}" linux-dirclean
fi
make "${make_args[@]}" -j"$jobs" 2>&1 | tee "$project/logs/connector3-diagnostic.log"
export PATH="$output/host/bin:$PATH"
images="$output/images"
"$output/host/bin/python3" "$output/build/optee-os-3.15.0/scripts/gen_tee_bin.py" \
    --input "$images/tee.elf" --out_tee_raw_bin "$images/tee-raw.bin"
cd "$images"
# Factory ft_board_setup() patches this exact path before entering Linux.
dtc -I dtb -O dts imx7d-galdrun-connector3-diag.dtb 2>/dev/null \
    | sed -e 's/^\([[:space:]]*\)bus@30800000 {/\1aips-bus@30800000 {/' \
          -e 's|/soc/bus@30800000/|/soc/aips-bus@30800000/|g' \
    | dtc -I dts -O dtb -o diagnostic.dtb - 2>/dev/null
fdtget diagnostic.dtb /soc/aips-bus@30800000/ethernet@30bf0000/mdio/ethernet-phy@0 reg >/dev/null
cat >diagnostic.its <<'EOF'
/dts-v1/;
/ {
    description = "Galdrun Connector 3 RAM diagnostics";
    #address-cells = <1>;
    images {
        kernel-1 {
            description = "Linux with diagnostic initramfs";
            data = /incbin/("zImage");
            type = "kernel"; arch = "arm"; os = "linux"; compression = "none";
            load = <0x80800000>; entry = <0x80800000>;
            hash-1 { algo = "sha256"; };
        };
        fdt-1 {
            description = "Connector 3 Ethernet diagnostics";
            data = /incbin/("diagnostic.dtb");
            type = "flat_dt"; arch = "arm"; compression = "none";
            load = <0x83000000>;
            hash-1 { algo = "sha256"; };
        };
        tee-1 {
            description = "Source-built OP-TEE";
            data = /incbin/("tee-raw.bin");
            type = "firmware"; arch = "arm"; compression = "none";
            load = <0x9fd00000>;
            hash-1 { algo = "sha256"; };
        };
    };
    configurations {
        default = "Connector";
        Connector {
            description = "Teufel Streamer RAM diagnostics";
            kernel = "kernel-1"; fdt = "fdt-1"; loadables = "tee-1";
            signature-1 {
                algo = "sha256,rsa4096"; key-name-hint = "verified-boot";
                sign-images = "kernel", "fdt", "loadables";
            };
        };
    };
};
EOF
printf '/dts-v1/; / {};\n' | dtc -I dts -O dtb -o diagnostic-control.dtb -
mkimage -f diagnostic.its -k "$signing" -K diagnostic-control.dtb -r diagnostic.itb
python3 "$project/scripts/verify-connector3-diagnostic.py" "$output"
destination="$project/output/connector3-diagnostic"
if [[ -f "$destination/hardware-verification.json" ]]; then
    mv -f "$destination/hardware-verification.json" "$destination/hardware-verification.previous.json"
fi
cp diagnostic.itb "$destination/connect3.img"
cp diagnostic-verification.json "$destination/"
mkdir -p "$destination/licenses"
rsync -a --delete "$output/legal-info/licenses/" "$destination/licenses/"
cp "$output/legal-info/manifest.csv" "$destination/licenses/manifest.csv"
# The bootloader reads at most 12 MiB; no ext2 recovery/installer payload follows.
truncate -s 12582912 "$destination/connect3.img"
cd "$destination"
sha256sum connect3.img >connect3.img.sha256
printf 'Experimental RAM-only image: %s/connect3.img\n' "$destination"
