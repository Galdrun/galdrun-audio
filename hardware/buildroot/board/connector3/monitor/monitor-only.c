// SPDX-License-Identifier: BSD-2-Clause
/* Connector 3 uses OP-TEE for its ARMv7 monitor/PSCI, without TEE services. */
#include <crypto/crypto.h>
#include <kernel/panic.h>
#include <rng_support.h>
#include <sm/optee_smc.h>
#include <tee/entry_std.h>
#include <tee/tee_cryp_utl.h>
#include <trace.h>

#if !defined(CFG_GALDRUN_MONITOR_ONLY) || !defined(CFG_PSCI_ARM32)
#error "Connector 3 requires the monitor-only PSCI profile"
#endif
#if defined(CFG_WITH_USER_TA) || defined(CFG_EARLY_TA) || \
    defined(CFG_CORE_ASLR) || defined(CFG_TA_ASLR) || \
    defined(CFG_WITH_PAGER) || defined(CFG_WITH_SOFTWARE_PRNG) || \
    defined(CFG_NXP_CAAM) || defined(CFG_IMX_CAAM) || defined(CFG_CRYPTO_DRIVER) || \
    defined(CFG_REE_FS) || defined(CFG_RPMB_FS)
#error "Monitor-only profile cannot enable TAs, RNG consumers or secure storage"
#endif

/* Override upstream's weak hooks. Never manufacture an RNG seed or bytes. */
void plat_rng_init(void)
{
    IMSG("Galdrun monitor-only: TEE sessions and RNG unavailable; PSCI enabled");
}

TEE_Result crypto_rng_init(const void *data __unused, size_t size __unused)
{
    return TEE_ERROR_NOT_SUPPORTED;
}

TEE_Result crypto_rng_read(void *data __unused, size_t size __unused)
{
    return TEE_ERROR_NOT_SUPPORTED;
}

uint8_t hw_get_random_byte(void)
{
    panic("Hardware RNG unavailable in monitor-only profile");
}

/* Reject all standard TEE commands, including every pseudo/user TA session. */
uint32_t tee_entry_std(struct optee_msg_arg *arg, uint32_t num_params __unused)
{
    arg->session = 0;
    arg->ret = TEE_ERROR_NOT_SUPPORTED;
    arg->ret_origin = TEE_ORIGIN_TEE;
    return OPTEE_SMC_RETURN_OK;
}
