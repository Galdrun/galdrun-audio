// SPDX-License-Identifier: GPL-2.0-only
/* Connector 2 clocking derived from Daniel Mack's vendor am33xx-s800.c.
 * Both output codecs share one physical I2S bus; capture uses CS4271.
 */
#include <linux/clk.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "davinci-mcasp.h"

struct connector2 {
    struct snd_soc_card card;
    struct snd_soc_dai_link link;
    struct snd_soc_dai_link_component cpu, platform, codecs[2];
    struct clk_bulk_data clocks[2];
    struct regulator *supply;
    struct mutex lock;
    unsigned long nominal_mclk;
    int drift;
};

static int connector2_set_clock(struct connector2 *priv, unsigned long nominal, int drift)
{
    /* Match vendor sign: positive compensation lowers the physical clock. */
    unsigned long actual = nominal - div_s64((s64)nominal * drift, 1000000);
    return clk_set_rate(priv->clocks[0].clk, actual);
}

static int connector2_hw_params(struct snd_pcm_substream *substream,
                                struct snd_pcm_hw_params *params)
{
    struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
    struct connector2 *priv = snd_soc_card_get_drvdata(rtd->card);
    struct snd_soc_dai *cpu = snd_soc_rtd_to_cpu(rtd, 0);
    unsigned int rate = params_rate(params);
    unsigned long mclk = rate % 11025 == 0 ? 22579200 : 24576000;
    int ret;

    if (params_channels(params) != 2 || mclk % (rate * 64))
        return -EINVAL;
    mutex_lock(&priv->lock);
    ret = connector2_set_clock(priv, mclk, priv->drift);
    if (ret)
        goto out;
    ret = snd_soc_dai_set_sysclk(cpu, MCASP_CLK_HCLK_AHCLK, mclk, SND_SOC_CLOCK_IN);
    if (ret)
        goto out;
    ret = snd_soc_dai_set_sysclk(snd_soc_rtd_to_codec(rtd, 0), 0, mclk, SND_SOC_CLOCK_IN);
    if (ret)
        goto out;
    /* AK4104 has no MCLK control interface; its sample rate follows I2S. */
    ret = snd_soc_dai_set_clkdiv(cpu, MCASP_CLKDIV_AUXCLK, 1);
    if (ret)
        goto out;
    ret = snd_soc_dai_set_clkdiv(cpu, MCASP_CLKDIV_BCLK, mclk / (rate * 64));
    if (ret)
        goto out;
    ret = snd_soc_dai_set_clkdiv(cpu, MCASP_CLKDIV_BCLK_FS_RATIO, 64);
    if (!ret)
        priv->nominal_mclk = mclk;
out:
    mutex_unlock(&priv->lock);
    return ret;
}

static int connector2_drift_info(struct snd_kcontrol *control, struct snd_ctl_elem_info *info)
{
    info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
    info->count = 1;
    info->value.integer.min = -500;
    info->value.integer.max = 500;
    return 0;
}

static int connector2_drift_get(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
    struct connector2 *priv = snd_soc_card_get_drvdata(snd_kcontrol_chip(control));
    mutex_lock(&priv->lock);
    value->value.integer.value[0] = priv->drift;
    mutex_unlock(&priv->lock);
    return 0;
}

static int connector2_drift_put(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
    struct connector2 *priv = snd_soc_card_get_drvdata(snd_kcontrol_chip(control));
    long drift = value->value.integer.value[0];
    int ret = 0;
    if (drift < -500 || drift > 500)
        return -EINVAL;
    mutex_lock(&priv->lock);
    if (drift != priv->drift) {
        ret = connector2_set_clock(priv, priv->nominal_mclk, drift);
        if (!ret) {
            priv->drift = drift;
            ret = 1;
        }
    }
    mutex_unlock(&priv->lock);
    return ret;
}

static const struct snd_kcontrol_new connector2_controls[] = {
    { .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Drift compensator",
      .info = connector2_drift_info, .get = connector2_drift_get, .put = connector2_drift_put },
};
static int connector2_startup(struct snd_pcm_substream *substream)
{
    /* Both codecs and the fixed 64-bit I2S frame require stereo. Constrain
     * during probing too, rather than rejecting mono only in hw_params. */
    return snd_pcm_hw_constraint_minmax(substream->runtime,
                                        SNDRV_PCM_HW_PARAM_CHANNELS, 2, 2);
}
static const struct snd_soc_ops connector2_ops = {
    .startup = connector2_startup, .hw_params = connector2_hw_params
};

static void connector2_release(void *data)
{
    struct connector2 *priv = data;
    clk_bulk_disable_unprepare(ARRAY_SIZE(priv->clocks), priv->clocks);
    regulator_disable(priv->supply);
    of_node_put(priv->cpu.of_node);
    of_node_put(priv->codecs[0].of_node);
    of_node_put(priv->codecs[1].of_node);
}

static int connector2_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct connector2 *priv;
    int ret;

    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;
    mutex_init(&priv->lock);
    priv->supply = devm_regulator_get(dev, "vd");
    if (IS_ERR(priv->supply))
        return dev_err_probe(dev, PTR_ERR(priv->supply), "audio supply unavailable\n");
    priv->clocks[0].id = "mclk";
    priv->clocks[1].id = "bus-mclk";
    ret = devm_clk_bulk_get(dev, ARRAY_SIZE(priv->clocks), priv->clocks);
    if (ret)
        return dev_err_probe(dev, ret, "audio clocks unavailable\n");
    ret = regulator_enable(priv->supply);
    if (ret)
        return ret;
    priv->nominal_mclk = 24576000;
    ret = clk_set_rate(priv->clocks[0].clk, priv->nominal_mclk);
    if (!ret)
        ret = clk_bulk_prepare_enable(ARRAY_SIZE(priv->clocks), priv->clocks);
    if (ret) {
        regulator_disable(priv->supply);
        return ret;
    }
    ret = devm_add_action_or_reset(dev, connector2_release, priv);
    if (ret)
        return ret;
    priv->cpu.of_node = of_parse_phandle(dev->of_node, "audio-cpu", 0);
    priv->platform.of_node = priv->cpu.of_node;
    priv->codecs[0].of_node = of_parse_phandle(dev->of_node, "audio-codecs", 0);
    priv->codecs[1].of_node = of_parse_phandle(dev->of_node, "audio-codecs", 1);
    if (!priv->cpu.of_node || !priv->codecs[0].of_node || !priv->codecs[1].of_node)
        return -EINVAL;
    priv->codecs[0].dai_name = "cs4271-hifi";
    priv->codecs[1].dai_name = "ak4104-hifi";
    priv->link.name = "Connector2 I2S";
    priv->link.stream_name = "Analog + S/PDIF";
    priv->link.cpus = &priv->cpu;
    priv->link.num_cpus = 1;
    priv->link.platforms = &priv->platform;
    priv->link.num_platforms = 1;
    priv->link.codecs = priv->codecs;
    priv->link.num_codecs = 2;
    priv->link.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
    priv->link.symmetric_rate = 1;
    priv->link.symmetric_sample_bits = 1;
    priv->link.ops = &connector2_ops;
    priv->card.owner = THIS_MODULE;
    priv->card.dev = dev;
    priv->card.name = "Connector2";
    priv->card.dai_link = &priv->link;
    priv->card.num_links = 1;
    priv->card.controls = connector2_controls;
    priv->card.num_controls = ARRAY_SIZE(connector2_controls);
    snd_soc_card_set_drvdata(&priv->card, priv);
    platform_set_drvdata(pdev, &priv->card);
    return devm_snd_soc_register_card(dev, &priv->card);
}

static const struct of_device_id connector2_match[] = {
    { .compatible = "raumfeld,connector2-audio" }, { }
};
MODULE_DEVICE_TABLE(of, connector2_match);
static struct platform_driver connector2_driver = {
    .probe = connector2_probe,
    .driver = { .name = "raumfeld-connector2-audio", .of_match_table = connector2_match,
                .pm = &snd_soc_pm_ops },
};
module_platform_driver(connector2_driver);
MODULE_DESCRIPTION("Custom Raumfeld Connector 2 audio");
MODULE_LICENSE("GPL");
