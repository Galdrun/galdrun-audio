"""Compile the real monitor hooks and verify unavailable services fail closed."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / 'board/connector3/monitor/monitor-only.c'
harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define __unused __attribute__((unused))
#define CFG_GALDRUN_MONITOR_ONLY 1
#define CFG_PSCI_ARM32 1
#define TEE_ERROR_NOT_SUPPORTED 0xffff000aU
#define TEE_ORIGIN_TEE 3U
#define OPTEE_SMC_RETURN_OK 0U
#define IMSG(...) ((void)0)
typedef uint32_t TEE_Result;
struct optee_msg_arg { uint32_t cmd, session, ret, ret_origin; };
static _Noreturn void panic(const char *message) { (void)message; exit(7); }
'''
main = r'''
int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) { hw_get_random_byte(); return 1; }
    uint8_t bytes[32];
    for (size_t n = 0; n < sizeof(bytes); n++) bytes[n] = 0x42;
    plat_rng_init();
    assert(crypto_rng_init(bytes, sizeof(bytes)) == TEE_ERROR_NOT_SUPPORTED);
    assert(crypto_rng_read(bytes, sizeof(bytes)) == TEE_ERROR_NOT_SUPPORTED);
    assert(crypto_rng_read(NULL, 0) == TEE_ERROR_NOT_SUPPORTED);
    for (size_t n = 0; n < sizeof(bytes); n++) assert(bytes[n] == 0x42);
    for (uint32_t cmd = 0; cmd < 16; cmd++) {
        struct optee_msg_arg arg = { .cmd=cmd, .session=123, .ret=0 };
        assert(tee_entry_std(&arg, 0) == OPTEE_SMC_RETURN_OK);
        assert(arg.session == 0 && arg.ret == TEE_ERROR_NOT_SUPPORTED);
        assert(arg.ret_origin == TEE_ORIGIN_TEE);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='connector3-monitor-only-') as directory:
    root = Path(directory)
    for header in ('crypto/crypto.h', 'kernel/panic.h', 'rng_support.h',
                   'sm/optee_smc.h', 'tee/entry_std.h', 'tee/tee_cryp_utl.h', 'trace.h'):
        path = root / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    test = root / 'test.c'
    test.write_text(harness + source.read_text() + main)
    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(test)]
    subprocess.run([*command, '-o', str(root/'test')], check=True)
    subprocess.run([str(root/'test')], check=True)
    assert subprocess.run([str(root/'test'), 'direct-hardware-call']).returncode == 7
    for option in ('CFG_WITH_USER_TA', 'CFG_EARLY_TA', 'CFG_CORE_ASLR', 'CFG_TA_ASLR',
                   'CFG_WITH_PAGER', 'CFG_WITH_SOFTWARE_PRNG', 'CFG_NXP_CAAM',
                   'CFG_IMX_CAAM', 'CFG_CRYPTO_DRIVER', 'CFG_REE_FS', 'CFG_RPMB_FS'):
        result = subprocess.run([*command, '-fsyntax-only', '-D'+option+'=1'], capture_output=True)
        assert result.returncode != 0 and b'Monitor-only profile cannot enable' in result.stderr, option
print('Monitor-only: TEE/RNG requests rejected, output untouched, direct RNG traps, incompatible configs rejected')
