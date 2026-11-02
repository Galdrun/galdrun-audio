// SPDX-License-Identifier: GPL-2.0-or-later
/* Connector 3 CPU-core power-down with DDR self-refresh through PSCI.
 * Keep SCU and peripheral clocks available for Ethernet and MCU UART wake.
 * GPC setup follows Freescale's i.MX7 WFI power-gating path. */
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/psci.h>
#include <linux/sizes.h>
#include <linux/suspend.h>
#include <asm/arch_timer.h>
#include <asm/mach/map.h>
#include <asm/suspend.h>
#include "common.h"

#define SOC_VIRT 0xf6200000
#define CNT_VIRT 0xf6300000
#define GPC ((void __iomem *)(SOC_VIRT + 0xa0000))
#define ANATOP ((void __iomem *)(SOC_VIRT + 0x60000))
#define SRC ((void __iomem *)(SOC_VIRT + 0x90000))
#define GPR ((void __iomem *)(SOC_VIRT + 0x40000))
#define COUNTER ((void __iomem *)(CNT_VIRT + 0xc0000))

static bool suspend_enabled;
static bool wake_clocks_held;
static struct clk_bulk_data wake_clocks[] = {
    { .id = "enet_out" }, { .id = "enet_clk_ref" }, { .id = "ipg" }, { .id = "ahb" },
};
void connector3_pm_map_io(void);
void connector3_pm_init(void);

static int __init connector3_suspend_setup(char *value)
{
    return kstrtobool(value, &suspend_enabled) == 0;
}
__setup("connector3_suspend=", connector3_suspend_setup);

static struct map_desc connector3_maps[] __initdata = {
    { .virtual = SOC_VIRT, .pfn = __phys_to_pfn(0x30300000),
      .length = SZ_1M, .type = MT_DEVICE },
    { .virtual = CNT_VIRT, .pfn = __phys_to_pfn(0x30600000),
      .length = SZ_1M, .type = MT_DEVICE },
};

void __init connector3_pm_map_io(void)
{
    if (of_flat_dt_is_compatible(of_get_flat_dt_root(), "fsl,imx7d-teufel-streamer"))
        iotable_init(connector3_maps, ARRAY_SIZE(connector3_maps));
}

static int connector3_finish(unsigned long unused)
{
    /* Power-down type, vendor suspend state ID0; firmware owns secure OCRAM. */
    return psci_ops.cpu_suspend(1U << 16, __pa_symbol(cpu_resume));
}

static int connector3_begin(suspend_state_t state)
{
    int ret = clk_bulk_prepare_enable(ARRAY_SIZE(wake_clocks), wake_clocks);
    wake_clocks_held = !ret;
    return ret;
}

static void connector3_end(void)
{
    if (wake_clocks_held) {
        clk_bulk_disable_unprepare(ARRAY_SIZE(wake_clocks), wake_clocks);
        wake_clocks_held = false;
    }
}

static int connector3_enter(suspend_state_t state)
{
    const unsigned int regs[] = { 0, 4, 8, 0x14, 0x20, 0x24, 0x890,
                                 0x804, 0x800, 0x880, 0xb0, 0xb8, 0xc8 };
    /* Preserve the qualified stock PLL override sequence. */
    const unsigned int pll_offsets[] = { 0x60, 0x70, 0xb0, 0xe0, 0xf0, 0x130 };
    const u32 pll_bits[] = { BIT(20), BIT(19), 0x1ff << 17, BIT(13), BIT(24), BIT(24) };
    u32 saved[ARRAY_SIZE(regs)], pll_saved[ARRAY_SIZE(pll_offsets)];
    u32 counter, gpr, wake_imr1, val, freq, kctl;
    unsigned int i;
    int ret;

    if (state != PM_SUSPEND_MEM || num_online_cpus() != 1 ||
        (readl_relaxed(SRC + 8) & BIT(1)) ||
        (readl_relaxed(GPC + 0xa00) & 1))
        return -EINVAL;
    for (i = 0; i < ARRAY_SIZE(regs); i++)
        saved[i] = readl_relaxed(GPC + regs[i]);
    for (i = 0; i < ARRAY_SIZE(pll_offsets); i++) {
        pll_saved[i] = readl_relaxed(ANATOP + pll_offsets[i]);
        writel_relaxed(pll_bits[i], ANATOP + pll_offsets[i] + 4);
    }
    counter = readl_relaxed(COUNTER);
    writel_relaxed((counter & ~BIT(8)) | BIT(9), COUNTER);
    gpr = readl_relaxed(GPR + 4);
    writel_relaxed(gpr | BIT(12), GPR + 4);

    /* Hold the pending IOMUX IRQ while selecting LPM: vendor GPC erratum. */
    wake_imr1 = readl_relaxed(GPC + 0x30);
    writel_relaxed(wake_imr1 & ~BIT(0), GPC + 0x30);
    dsb();
    val = readl_relaxed(GPC);
    writel_relaxed((val & ~(0xf | BIT(14))) | 0x70000005, GPC);
    writel_relaxed(readl_relaxed(GPC + 8) | BIT(31), GPC + 8);
    val = readl_relaxed(GPC + 0x14);
    val &= ~(BIT(31) | BIT(30) | 0x3f000000 | BIT(16) | BIT(2) | BIT(1) | BIT(0));
    writel_relaxed(val | BIT(16), GPC + 0x14);
    writel_relaxed(wake_imr1, GPC + 0x30);
    dsb();

    /* Gate only CPU0 on WFI, wake on IRQ; retain SCU and peripheral domain. */
    writel_relaxed((readl_relaxed(GPC + 4) & ~(0xf0f | BIT(16) | BIT(4))) |
                    BIT(8) | BIT(0), GPC + 4);
    writel_relaxed((0x59 << 10) | 0x5b | (2 << 20), GPC + 0x890);
    writel_relaxed((readl_relaxed(GPC + 0x804) & ~0x7fff80) | (0x1a << 7), GPC + 0x804);
    writel_relaxed(0, GPC + 0xb0);
    writel_relaxed(0, GPC + 0xb8);
    writel_relaxed(0, GPC + 0xc8);
    writel_relaxed(BIT(31) | BIT(15), GPC + 0x24);
    writel_relaxed(1, GPC + 0x800);
    writel_relaxed(0, GPC + 0x880);
    dsb();

    freq = arch_timer_get_cntfrq();
    kctl = arch_timer_get_cntkctl();
    ret = cpu_pm_enter();
    if (!ret) {
        ret = cpu_suspend(0, connector3_finish);
        cpu_pm_exit();
    }
    if (!ret && (!freq || freq != arch_timer_get_cntfrq() ||
                 kctl != arch_timer_get_cntkctl())) {
        pr_err("Connector3 resume: architectural timer context was not restored\n");
        ret = -EIO;
    }
    writel_relaxed(0, SRC + 0x74);
    writel_relaxed(0, SRC + 0x78);
    for (i = 0; i < ARRAY_SIZE(regs); i++)
        writel_relaxed(saved[i], GPC + regs[i]);
    udelay(65);
    writel_relaxed(gpr, GPR + 4);
    writel_relaxed(counter, COUNTER);
    for (i = 0; i < ARRAY_SIZE(pll_offsets); i++)
        writel_relaxed(pll_bits[i] & ~pll_saved[i], ANATOP + pll_offsets[i] + 8);
    pr_info("Connector3 PSCI suspend/resume: result=%d\n", ret);
    return ret;
}

static const struct platform_suspend_ops connector3_suspend_ops = {
    .valid = suspend_valid_only_mem, .begin = connector3_begin,
    .enter = connector3_enter, .end = connector3_end,
};

void __init connector3_pm_init(void)
{
    struct device_node *ethernet;
    unsigned int i;

    if (!of_machine_is_compatible("fsl,imx7d-teufel-streamer") ||
        !(suspend_enabled || of_property_read_bool(of_root, "galdrun,cpu-core-suspend") ||
          of_property_read_bool(of_root, "galdrun,experimental-suspend")))
        return;
    if (!psci_ops.cpu_suspend) {
        pr_err("Connector3 suspend requires the PSCI monitor\n");
        return;
    }
    /* Firmware may leave CPU1 enabled although Linux boots with maxcpus=1. */
    if (cpu_possible(1) && !cpu_online(1))
        imx_enable_cpu(1, false);
    if (num_online_cpus() != 1 || (readl_relaxed(SRC + 8) & BIT(1))) {
        pr_err("Connector3 suspend requires CPU1 to be parked offline\n");
        return;
    }
    ethernet = of_find_node_by_path("ethernet0");
    if (!ethernet) {
        pr_err("Connector3 suspend: Ethernet clock provider unavailable\n");
        return;
    }
    for (i = 0; i < ARRAY_SIZE(wake_clocks); i++)
        wake_clocks[i].clk = of_clk_get_by_name(ethernet, wake_clocks[i].id);
    of_node_put(ethernet);
    for (i = 0; i < ARRAY_SIZE(wake_clocks); i++)
        if (IS_ERR(wake_clocks[i].clk))
            goto clocks_unavailable;
    suspend_set_ops(&connector3_suspend_ops);
    pr_info("Connector3 PSCI CPU-core suspend enabled; peripheral wake retained\n");
    return;

clocks_unavailable:
    for (i = 0; i < ARRAY_SIZE(wake_clocks); i++)
        if (!IS_ERR_OR_NULL(wake_clocks[i].clk))
            clk_put(wake_clocks[i].clk);
    pr_err("Connector3 suspend: Ethernet wake clocks unavailable\n");
}
