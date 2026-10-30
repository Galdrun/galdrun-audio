// SPDX-License-Identifier: GPL-2.0-only
/* S810 clock families verified against Teufel's published imx-s810.c. */
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

struct connector3_audio {
    struct snd_soc_card card;
    struct snd_soc_dai_link link;
    struct snd_soc_dai_link_component cpu, platform, codec;
    struct clk_bulk_data clocks[3];
    bool clocks_enabled;
    struct mutex lock;
    unsigned long nominal_pll;
    int drift;
};

static int connector3_drift_info(struct snd_kcontrol *control, struct snd_ctl_elem_info *info)
{
    info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
    info->count = 1;
    info->value.integer.min = -500;
    info->value.integer.max = 500;
    info->value.integer.step = 1;
    return 0;
}

static int connector3_drift_get(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
    struct connector3_audio *priv = snd_soc_card_get_drvdata(snd_kcontrol_chip(control));
    mutex_lock(&priv->lock);
    value->value.integer.value[0] = priv->drift;
    mutex_unlock(&priv->lock);
    return 0;
}

static int connector3_drift_put(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
    struct connector3_audio *priv = snd_soc_card_get_drvdata(snd_kcontrol_chip(control));
    long drift = value->value.integer.value[0];
    unsigned long rate;
    int ret = 0;

    if (drift < -500 || drift > 500)
        return -EINVAL;
    mutex_lock(&priv->lock);
    if (drift == priv->drift)
        goto out;
    /* Stock S810 convention: positive ppm slows the DAC. Retune only the
     * fractional PLL; both root dividers remain fixed and PCM keeps running. */
    rate = priv->nominal_pll - div_s64((s64)priv->nominal_pll * drift, 1000000);
    ret = clk_set_rate(priv->clocks[0].clk, rate);
    if (!ret && clk_get_rate(priv->clocks[0].clk) != rate)
        ret = -ERANGE;
    if (ret) {
        clk_set_rate(priv->clocks[0].clk, priv->nominal_pll -
                     div_s64((s64)priv->nominal_pll * priv->drift, 1000000));
    } else {
        priv->drift = drift;
        ret = 1;
    }
out:
    mutex_unlock(&priv->lock);
    return ret;
}

static const struct snd_kcontrol_new connector3_controls[] = {
    { .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Drift compensator",
      .info = connector3_drift_info, .get = connector3_drift_get, .put = connector3_drift_put },
};

static const unsigned int connector3_rates[] = {
    22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000
};

static const struct snd_pcm_hw_constraint_list connector3_rate_list = {
    .count = ARRAY_SIZE(connector3_rates), .list = connector3_rates,
};

static int connector3_audio_startup(struct snd_pcm_substream *substream)
{
    int ret = snd_pcm_hw_constraint_minmax(substream->runtime,
                                          SNDRV_PCM_HW_PARAM_CHANNELS, 2, 2);
    if (ret)
        return ret;
    return snd_pcm_hw_constraint_list(substream->runtime, 0,
                                      SNDRV_PCM_HW_PARAM_RATE, &connector3_rate_list);
}

static int connector3_audio_hw_params(struct snd_pcm_substream *substream,
                                      struct snd_pcm_hw_params *params)
{
    struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
    struct connector3_audio *priv = snd_soc_card_get_drvdata(rtd->card);
    struct snd_soc_dai *cpu = snd_soc_rtd_to_cpu(rtd, 0);
    unsigned int rate = params_rate(params);
    unsigned long mclk = rate % 11025 == 0 ? 22579200 : 24576000;
    int ret, enable_ret;

    if (params_channels(params) != 2 || mclk % (rate * 64))
        return -EINVAL;
    mutex_lock(&priv->lock);
    /* Retune the shared PLL first: changing only the root divider rounds
     * 22.5792 MHz to 22.1184 MHz while the PLL remains at 884.736 MHz.
     * Both SAI and the carrier-board master clock use PLL / 36.
     */
    /* Mainline's prepared audio post-dividers protect their entire parent
     * tree against rate changes. No PCM is running at this point: release
     * our references before switching the PLL family, then restore them.
     */
    if (priv->clocks_enabled) {
        clk_bulk_disable_unprepare(ARRAY_SIZE(priv->clocks), priv->clocks);
        priv->clocks_enabled = false;
    }
    ret = clk_set_rate(priv->clocks[0].clk, mclk * 36);
    if (!ret)
        ret = clk_set_rate(priv->clocks[1].clk, mclk);
    if (!ret)
        ret = clk_set_rate(priv->clocks[2].clk, mclk);
    enable_ret = clk_bulk_prepare_enable(ARRAY_SIZE(priv->clocks), priv->clocks);
    priv->clocks_enabled = !enable_ret;
    if (ret || enable_ret) {
        ret = ret ? ret : enable_ret;
        goto out;
    }
    if (clk_get_rate(priv->clocks[0].clk) != mclk * 36 ||
        clk_get_rate(priv->clocks[1].clk) != mclk ||
        clk_get_rate(priv->clocks[2].clk) != mclk)
    {
        ret = -EINVAL;
        goto out;
    }
    priv->nominal_pll = mclk * 36;
    priv->drift = 0;
    ret = snd_soc_dai_set_tdm_slot(cpu, 3, 3, 2, 32);
    if (ret)
        goto out;
    ret = snd_soc_dai_set_sysclk(cpu, 1, mclk, SND_SOC_CLOCK_IN);
out:
    mutex_unlock(&priv->lock);
    return ret;
}

static const struct snd_soc_ops connector3_audio_ops = {
    .startup = connector3_audio_startup, .hw_params = connector3_audio_hw_params,
};

static void connector3_audio_release(void *data)
{
    struct connector3_audio *priv = data;
    if (priv->clocks_enabled)
        clk_bulk_disable_unprepare(ARRAY_SIZE(priv->clocks), priv->clocks);
    of_node_put(priv->cpu.of_node);
    of_node_put(priv->codec.of_node);
}

static int connector3_audio_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct connector3_audio *priv;
    int ret;

    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;
    mutex_init(&priv->lock);
    priv->clocks[0].id = "pll";
    priv->clocks[1].id = "mclk";
    priv->clocks[2].id = "sai";
    ret = devm_clk_bulk_get(dev, ARRAY_SIZE(priv->clocks), priv->clocks);
    if (ret)
        return dev_err_probe(dev, ret, "audio clocks unavailable\n");
    ret = clk_bulk_prepare_enable(ARRAY_SIZE(priv->clocks), priv->clocks);
    if (ret)
        return ret;
    priv->clocks_enabled = true;
    priv->nominal_pll = clk_get_rate(priv->clocks[0].clk);
    priv->cpu.of_node = of_parse_phandle(dev->of_node, "audio-cpu", 0);
    priv->platform.of_node = priv->cpu.of_node;
    priv->codec.of_node = of_parse_phandle(dev->of_node, "audio-codec", 0);
    ret = devm_add_action_or_reset(dev, connector3_audio_release, priv);
    if (ret)
        return ret;
    if (!priv->cpu.of_node || !priv->codec.of_node)
        return -EINVAL;
    priv->codec.dai_name = "dit-hifi";
    priv->link.name = "Connector3 I2S";
    priv->link.stream_name = "Analog";
    priv->link.cpus = &priv->cpu;
    priv->link.num_cpus = 1;
    priv->link.platforms = &priv->platform;
    priv->link.num_platforms = 1;
    priv->link.codecs = &priv->codec;
    priv->link.num_codecs = 1;
    priv->link.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
    priv->link.ops = &connector3_audio_ops;
    priv->card.owner = THIS_MODULE;
    priv->card.dev = dev;
    priv->card.name = "Connector3";
    priv->card.dai_link = &priv->link;
    priv->card.num_links = 1;
    priv->card.controls = connector3_controls;
    priv->card.num_controls = ARRAY_SIZE(connector3_controls);
    snd_soc_card_set_drvdata(&priv->card, priv);
    platform_set_drvdata(pdev, &priv->card);
    return devm_snd_soc_register_card(dev, &priv->card);
}

static const struct of_device_id connector3_audio_match[] = {
    { .compatible = "galdrun,connector3-audio" }, { }
};
MODULE_DEVICE_TABLE(of, connector3_audio_match);
static struct platform_driver connector3_audio_driver = {
    .probe = connector3_audio_probe,
    .driver = { .name = "galdrun-connector3-audio", .of_match_table = connector3_audio_match },
};
module_platform_driver(connector3_audio_driver);
MODULE_DESCRIPTION("Galdrun Connector 3 audio clock switching");
MODULE_LICENSE("GPL");
