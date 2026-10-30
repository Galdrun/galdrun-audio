// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2015 Freescale Semiconductor, Inc.
 * Connector 3 CPU-off / DDR self-refresh, adapted from Freescale's 2015
 * pm-imx7.c and gpcv2.c. Peripheral domain retained for Ethernet wake.
 * Deliberately no LPSR, PMIC rail changes or DDR PHY power cycling. */
#include <linux/cpu.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/sizes.h>
#include <linux/suspend.h>
#include <asm/cacheflush.h>
#include <asm/fncpy.h>
#include <asm/mach/map.h>
#include <asm/pgtable.h>
#include <asm/suspend.h>
#include <asm/tlbflush.h>

#define SRAM_PHYS 0x00900000
#define SRAM_VIRT 0xf6000000
#define DDR_VIRT  0xf6100000
#define SOC_VIRT  0xf6200000
#define CNT_VIRT  0xf6300000
#define GPC ((void __iomem *)(SOC_VIRT + 0xa0000))
#define ANATOP ((void __iomem *)(SOC_VIRT + 0x60000))
#define COUNTER ((void __iomem *)(CNT_VIRT + 0xc0000))

/* The upper 8 KiB hold TTBR1 entries. Code starts at byte 0x100. */
struct connector3_pm_info {
    u32 pbase, resume, ddrc_phys, ddrc_virt, src_phys, src_virt;
    u32 ttbr1, ttbcr, sctlr, resumed;
};
static struct connector3_pm_info * const info = (void *)SRAM_VIRT;
extern void connector3_suspend(void *info);
extern char connector3_suspend_end[];
extern void v7_cpu_resume(void);
static void (*suspend_fn)(void *);
static bool suspend_enabled;
static struct clk_bulk_data wake_clocks[] = {
    { .id = "enet_out" }, { .id = "enet_clk_ref" }, { .id = "ipg" }, { .id = "ahb" },
};
static void __iomem *fec_regs;
static bool wake_clocks_held;
void connector3_pm_map_io(void);
void connector3_pm_init(void);

/* Explicit opt-in during qualification; an untested port must not replace
 * another platform's suspend ops or make the appliance claim deep sleep. */
static int __init connector3_suspend_setup(char *value)
{
    return kstrtobool(value, &suspend_enabled) == 0;
}
__setup("connector3_suspend=", connector3_suspend_setup);

static struct map_desc connector3_maps[] __initdata = {
    { .virtual = SRAM_VIRT, .pfn = __phys_to_pfn(SRAM_PHYS),
      .length = SZ_16K, .type = MT_MEMORY_RWX_NONCACHED },
    { .virtual = DDR_VIRT, .pfn = __phys_to_pfn(0x30700000),
      .length = SZ_1M, .type = MT_DEVICE },
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
    local_flush_tlb_all();
    suspend_fn(info);
    return 0;
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
    const unsigned int pll_offsets[] = { 0x60, 0x70, 0xb0, 0xe0, 0xf0, 0x130 };
    const u32 pll_bits[] = { BIT(20), BIT(19), 0x1ff << 17, BIT(13), BIT(24), BIT(24) };
    u32 saved[ARRAY_SIZE(regs)], pll_saved[ARRAY_SIZE(pll_offsets)];
    u32 counter, gpr, val;
    unsigned int i;
    int ret;

    if (state != PM_SUSPEND_MEM || num_online_cpus() != 1 ||
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
    gpr = readl_relaxed((void __iomem *)(SOC_VIRT + 0x40004));
    writel_relaxed(gpr | BIT(12), (void __iomem *)(SOC_VIRT + 0x40004));

    /* Mainline's GPC IRQ syscore_ops have already installed wake-only IMRs.
     * Both A7 LPM fields select STOP; M4 does not participate in this port. */
    val = readl_relaxed(GPC);
    writel_relaxed((val & ~(0xf | BIT(14))) | 0x7000000a, GPC);
    writel_relaxed(readl_relaxed(GPC + 8) | BIT(31), GPC + 8);
    val = readl_relaxed(GPC + 0x14);
    /* Keep the 24 MHz oscillator and external supplies on: the LAN8720's
     * SoC-generated reference must run while its magic-packet detector is
     * armed. CPU0 and SCU are still gated by their PGC/slot configuration. */
    val &= ~(0x3f000000 | BIT(16) | BIT(2) | BIT(1));
    val |= BIT(31) | BIT(30) | BIT(0);
    writel_relaxed(val, GPC + 0x14);
    writel_relaxed((readl_relaxed(GPC + 4) & ~0xf0f) |
                    BIT(16) | BIT(9) | BIT(4) | BIT(1), GPC + 4);
    writel_relaxed((0x59 << 10) | 0x5b | (2 << 20), GPC + 0x890);
    writel_relaxed((readl_relaxed(GPC + 0x804) & ~0x7fff80) | (0x1a << 7), GPC + 0x804);
    /* CORE0 down, SCU down+ack; SCU up, CORE0 up+ack. */
    writel_relaxed(1, GPC + 0xb0);
    writel_relaxed(1 << 4, GPC + 0xb8);
    writel_relaxed((2 << 4) | 2, GPC + 0xc8);
    writel_relaxed(BIT(2) | BIT(16), GPC + 0x24);
    writel_relaxed(1, GPC + 0x800);
    writel_relaxed(1, GPC + 0x880);
    info->resumed = 0;
    pr_info("Connector3 wake: ECR=%#x EIR=%#x EIMR=%#x IMR=%08x/%08x/%08x/%08x\n",
            readl_relaxed(fec_regs + 0x24), readl_relaxed(fec_regs + 4),
            readl_relaxed(fec_regs + 8), readl_relaxed(GPC + 0x30),
            readl_relaxed(GPC + 0x34), readl_relaxed(GPC + 0x38), readl_relaxed(GPC + 0x3c));
    dsb();
    ret = cpu_suspend(0, connector3_finish);
    pr_info("Connector3 wake: resumed EIR=%#x\n", readl_relaxed(fec_regs + 4));
    writel_relaxed(0, (void __iomem *)(SOC_VIRT + 0x90074));
    writel_relaxed(0, (void __iomem *)(SOC_VIRT + 0x90078));

    /* Restore the exact entry state, including pre-existing PLL overrides. */
    for (i = 0; i < ARRAY_SIZE(regs); i++)
        writel_relaxed(saved[i], GPC + regs[i]);
    /* RBC changes require at least two CKIL cycles before IMRs are restored. */
    udelay(65);
    writel_relaxed(gpr, (void __iomem *)(SOC_VIRT + 0x40004));
    writel_relaxed(counter, COUNTER);
    for (i = 0; i < ARRAY_SIZE(pll_offsets); i++)
        writel_relaxed(pll_bits[i] & ~pll_saved[i], ANATOP + pll_offsets[i] + 8);
    pr_info("Connector3 suspend: CPU-off resume=%u, result=%d\n", info->resumed, ret);
    return ret;
}

static const struct platform_suspend_ops connector3_suspend_ops = {
    .valid = suspend_valid_only_mem, .begin = connector3_begin,
    .enter = connector3_enter, .end = connector3_end,
};

void __init connector3_pm_init(void)
{
    u32 *table = (void *)SRAM_VIRT;
    unsigned int i;
    struct device_node *ethernet;
    size_t code_size = connector3_suspend_end - (char *)connector3_suspend;

    BUILD_BUG_ON(sizeof(*info) > 0x100);
    BUILD_BUG_ON(offsetof(struct connector3_pm_info, resumed) != 36);
    if (!of_machine_is_compatible("fsl,imx7d-teufel-streamer") ||
        !(suspend_enabled || of_property_read_bool(of_root, "galdrun,experimental-suspend")))
        return;
    if (IS_ENABLED(CONFIG_ARM_LPAE) || code_size > 0x1f00) {
        pr_err("Connector3 suspend: unsupported page tables or OCRAM code size\n");
        return;
    }
    /* The LAN8720 takes its 50 MHz reference from this SoC. FEC releases
     * these references in suspend even with WOL armed; retain them across
     * device suspend so the PHY and magic-packet detector keep their clocks. */
    ethernet = of_find_node_by_path("ethernet0");
    for (i = 0; i < ARRAY_SIZE(wake_clocks); i++)
        wake_clocks[i].clk = of_clk_get_by_name(ethernet, wake_clocks[i].id);
    fec_regs = of_iomap(ethernet, 0);
    of_node_put(ethernet);
    for (i = 0; i < ARRAY_SIZE(wake_clocks); i++)
        if (IS_ERR(wake_clocks[i].clk)) goto clocks_unavailable;
    if (!fec_regs) goto clocks_unavailable;
    memset((void *)SRAM_VIRT, 0, SZ_16K);
    for (i = 0; i < ARRAY_SIZE(connector3_maps); i++) {
        u32 phys = __pfn_to_phys(connector3_maps[i].pfn);
        u32 attributes = connector3_maps[i].type == MT_DEVICE ?
                         PMD_SECT_BUFFERABLE | PMD_SECT_XN : PMD_SECT_TEX(1);
        table[connector3_maps[i].virtual >> 20] = phys | PMD_TYPE_SECT |
                PMD_SECT_AP_WRITE | attributes | PMD_SECT_S;
    }
    info->pbase = SRAM_PHYS;
    info->resume = __pa_symbol(v7_cpu_resume);
    info->ddrc_phys = 0x307a0000;
    info->ddrc_virt = DDR_VIRT + 0xa0000;
    info->src_phys = 0x30390000;
    info->src_virt = SOC_VIRT + 0x90000;
    suspend_fn = fncpy((void *)(SRAM_VIRT + 0x100), &connector3_suspend, code_size);
    suspend_set_ops(&connector3_suspend_ops);
    pr_info("Connector3 experimental CPU-off/DDR self-refresh suspend enabled\n");
    return;
clocks_unavailable:
    for (i = 0; i < ARRAY_SIZE(wake_clocks); i++)
        if (!IS_ERR(wake_clocks[i].clk)) clk_put(wake_clocks[i].clk);
    if (fec_regs) iounmap(fec_regs);
    pr_err("Connector3 suspend: Ethernet wake resources unavailable\n");
}
