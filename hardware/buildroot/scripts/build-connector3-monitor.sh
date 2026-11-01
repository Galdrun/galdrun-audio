#!/usr/bin/env bash
# Separate monitor candidates. Does not alter the qualified storage USB image.
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
[[ $# == 0 || ( $# == 1 && $1 == --production ) ]] || { echo "Usage: $0 [--production]" >&2; exit 2; }
profile=candidate
[[ $# == 0 ]] || profile=production
source "$project/PrepareHost.sh"
jobs=${JOBS:-16}
[[ $jobs =~ ^[0-9]+$ && $jobs -ge 1 && $jobs -le 32 ]] || exit 2
exec 9>"$work/build.lock"
flock -n 9 || { echo 'Another firmware build is running.' >&2; exit 1; }
diagnostic="$work/connector3-diagnostic"
export PATH="$diagnostic/host/bin:$PATH"
source_tree="$diagnostic/build/optee-os-3.15.0"
output="$work/connector3-monitor"
destination="$project/output/connector3-monitor"
extra=()
crypto=(CFG_NXP_CAAM=y CFG_CRYPTO_DRIVER=y CFG_DBG_CAAM_TRACE=0x63)
if [[ $profile == production ]]; then
    output="$work/connector3-monitor-production-only"
    destination="$project/output/connector3-monitor-production"
    source_tree="$work/connector3-monitor-production-only-source"
    if [[ ! -f "$source_tree/Makefile" ]]; then
        cp -a --reflink=auto "$diagnostic/build/optee-os-3.15.0" "$source_tree"
    fi
    cp "$project/board/connector3/monitor/monitor-only.c" "$source_tree/core/arch/arm/plat-imx/monitor-only.c"
    if ! grep -q 'monitor-only.c' "$source_tree/core/arch/arm/plat-imx/sub.mk"; then
        echo 'srcs-$(CFG_GALDRUN_MONITOR_ONLY) += monitor-only.c' >>"$source_tree/core/arch/arm/plat-imx/sub.mk"
    fi
    if ! grep -Fq 'write_cntfrq(cntfrq);' "$source_tree/core/arch/arm/plat-imx/pm/imx7_suspend.c"; then
        patch -d "$source_tree" -p1 <"$project/board/connector3/monitor/restore-cntfrq.patch"
    fi
    crypto=(CFG_NXP_CAAM=n CFG_IMX_CAAM=n CFG_CRYPTO_DRIVER=n CFG_WITH_SOFTWARE_PRNG=n)
    extra=(CFG_GALDRUN_MONITOR_ONLY=y CFG_WITH_USER_TA=n CFG_EARLY_TA=n
           CFG_TA_ASLR=n CFG_REE_FS_TA=n CFG_DEVICE_ENUM_PTA=n CFG_GP_SOCKETS=n)
fi
mkdir -p "$output" "$destination" "$project/logs"
make -C "$source_tree" "O=$output" -j"$jobs" \
    PLATFORM=imx PLATFORM_FLAVOR=mx7dsabresd \
    "CROSS_COMPILE=$diagnostic/host/bin/arm-linux-" \
    LDFLAGS=-znoexecstack CFG_WERROR=n CFG_DDR_SIZE=0x20000000 \
    CFG_TZDRAM_START=0x9fd00000 CFG_TZDRAM_SIZE=0x300000 \
    CFG_SHMEM_START=0x9fc00000 CFG_SHMEM_SIZE=0x100000 CFG_NSEC_DDR_0_SIZE=0x1fc00000 \
    CFG_DT=n CFG_DT_ADDR=0 CFG_NS_ENTRY_ADDR= CFG_TEE_CORE_LOG_LEVEL=3 \
    CFG_CORE_ASLR=n CFG_WITH_PAGER=n CFG_RPMB_FS=n CFG_REE_FS=n \
    "${crypto[@]}" "${extra[@]}" \
    >"$project/logs/connector3-monitor.log" 2>&1
"$diagnostic/host/bin/python3" "$source_tree/scripts/gen_tee_bin.py" \
    --input "$output/core/tee.elf" --out_tee_raw_bin "$output/tee-raw.bin"
python3 - "$output" "$destination" "$profile" <<'PY'
from pathlib import Path
import hashlib,json,shutil,sys
output,destination=map(Path,sys.argv[1:3]);profile=sys.argv[3]
config=(output/'include/generated/conf.h').read_text().splitlines()
required_config=['/* CFG_DT is not set */','/* CFG_WITH_SOFTWARE_PRNG is not set */']
if profile=='production':
    required_config += ['#define CFG_GALDRUN_MONITOR_ONLY 1','#define CFG_PSCI_ARM32 1',
        '/* CFG_NXP_CAAM is not set */','/* CFG_IMX_CAAM is not set */',
        '/* CFG_WITH_USER_TA is not set */',
        '/* CFG_EARLY_TA is not set */','/* CFG_TA_ASLR is not set */']
else:
    required_config += ['#define CFG_NXP_CAAM 1','#define CFG_NXP_CAAM_RNG_DRV 1']
for required in required_config:
    if required not in config: raise ValueError('Monitor config missing: '+required)
for name,source in (('tee-raw.bin',output/'tee-raw.bin'),('tee.elf',output/'core/tee.elf'),
                    ('conf.h',output/'include/generated/conf.h')):
    shutil.copyfile(source,destination/name)
report={'purpose':'hardware CAAM RNG/static-platform monitor candidate',
    'rng_driver':'NXP CAAM; hardware-backed RNG, software PRNG disabled',
    'device_tree_required':False,'hardware_verified':False,'production_qualified':False,
    'sha256':hashlib.sha256((destination/'tee-raw.bin').read_bytes()).hexdigest()}
if profile=='production':
    import subprocess
    symbols=subprocess.check_output([str(output.parent/'connector3-diagnostic/host/bin/arm-linux-nm'),
                                     str(output/'core/tee.elf')],text=True).splitlines()
    definitions={parts[-1]:parts[-2] for line in symbols if len(parts:=line.split())>=3}
    for name in ('plat_rng_init','crypto_rng_read','tee_entry_std'):
        if definitions.get(name)!='T':
            raise ValueError('Monitor fail-closed override missing: '+name)
    if any(name.startswith('caam_') or name in ('crypto_driver_init','init_caam') for name in definitions):
        raise ValueError('Monitor-only binary must not include the CAAM driver')
    report['purpose']='ARMv7 monitor/PSCI candidate; all standard TEE commands rejected'
    report['rng_driver']=None
    report['rng_requests_rejected']=True
    report['tee_services_disabled']=True
    report['caam_access_disabled']=True
    report['user_ta_loading_disabled']=True
    report['arm_service_overrides_verified']=True
    report['cpu_suspend_timer_frequency_restored']=True
(destination/'monitor-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
PY
