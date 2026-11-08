// SPDX-License-Identifier: GPL-2.0-only
/* S810 clock families verified against Teufel's published imx-s810.c. */
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* One PCM, "Analog + Digital", like Connector 2's shared I2S bus: SAI1 to the
 * RCA DAC and SAI2 to the TOSLINK transmitter play the same samples. The two
 * SAIs have separate DMA requests, and the generic dmaengine PCM serves one
 * channel per substream, so this driver's own PCM component runs one cyclic
 * transfer per SAI from the one buffer, started and stopped together; the
 * position comes from SAI1. Both SAIs run from the same PLL. */
#define CONNECTOR3_LINKS 1
#define CONNECTOR3_SAIS 2

struct connector3_audio {
    struct snd_soc_card card;
    struct snd_soc_dai_link links[CONNECTOR3_LINKS];
    struct snd_soc_dai_link_component cpus[CONNECTOR3_SAIS], platforms[CONNECTOR3_LINKS],
                                      codecs[CONNECTOR3_SAIS];
    struct dma_chan *dma[CONNECTOR3_SAIS]; /* SAI1 TX, SAI2 TX */
    dma_cookie_t cookie;                   /* SAI1's transfer, for the position */
    struct clk_bulk_data clocks[4];
    bool clocks_enabled;
    struct mutex lock;
    unsigned long nominal_pll;
    unsigned long mclk;
    unsigned int configured; /* links with hw_params applied, one bit each */
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
    struct snd_soc_dai *cpu;
    unsigned int rate = params_rate(params);
    unsigned long mclk = rate % 11025 == 0 ? 22579200 : 24576000;
    unsigned int self = BIT(rtd->num);
    int i, ret, enable_ret;

    if (params_channels(params) != 2 || mclk % (rate * 64))
        return -EINVAL;
    mutex_lock(&priv->lock);
    /* Both SAIs share the PLL. While the other output is configured, join it
     * only in the same clock family and leave the running clocks untouched. */
    if (priv->configured & ~self) {
        if (mclk != priv->mclk) {
            ret = -EBUSY;
            goto out;
        }
        goto configure_dai;
    }
    priv->configured &= ~self;
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
    if (!ret)
        ret = clk_set_rate(priv->clocks[3].clk, mclk);
    enable_ret = clk_bulk_prepare_enable(ARRAY_SIZE(priv->clocks), priv->clocks);
    priv->clocks_enabled = !enable_ret;
    if (ret || enable_ret) {
        ret = ret ? ret : enable_ret;
        goto out;
    }
    if (clk_get_rate(priv->clocks[0].clk) != mclk * 36 ||
        clk_get_rate(priv->clocks[1].clk) != mclk ||
        clk_get_rate(priv->clocks[2].clk) != mclk ||
        clk_get_rate(priv->clocks[3].clk) != mclk)
    {
        ret = -EINVAL;
        goto out;
    }
    priv->nominal_pll = mclk * 36;
    priv->mclk = mclk;
    priv->drift = 0;
configure_dai:
    for_each_rtd_cpu_dais(rtd, i, cpu) {
        ret = snd_soc_dai_set_tdm_slot(cpu, 3, 3, 2, 32);
        if (!ret)
            ret = snd_soc_dai_set_sysclk(cpu, 1, mclk, SND_SOC_CLOCK_IN);
        if (ret)
            goto out;
    }
    priv->configured |= self;
out:
    mutex_unlock(&priv->lock);
    return ret;
}

static int connector3_audio_hw_free(struct snd_pcm_substream *substream)
{
    struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
    struct connector3_audio *priv = snd_soc_card_get_drvdata(rtd->card);

    mutex_lock(&priv->lock);
    priv->configured &= ~BIT(rtd->num);
    mutex_unlock(&priv->lock);
    return 0;
}

static const struct snd_soc_ops connector3_audio_ops = {
    .startup = connector3_audio_startup, .hw_params = connector3_audio_hw_params,
    .hw_free = connector3_audio_hw_free,
};

static const struct snd_pcm_hardware connector3_pcm_hardware = {
    .info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED |
            SNDRV_PCM_INFO_BLOCK_TRANSFER,
    .formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,
    .channels_min = 2, .channels_max = 2,
    .period_bytes_min = 256, .period_bytes_max = 128 * 1024,
    .periods_min = 2, .periods_max = 255,
    .buffer_bytes_max = 1024 * 1024,
};

static struct connector3_audio *connector3_from(struct snd_soc_component *component)
{
    return snd_soc_card_get_drvdata(component->card);
}

static int connector3_pcm_open(struct snd_soc_component *component,
                               struct snd_pcm_substream *substream)
{
    if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
        return -EINVAL;
    snd_soc_set_runtime_hwparams(substream, &connector3_pcm_hardware);
    return snd_pcm_hw_constraint_integer(substream->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
}

static int connector3_pcm_hw_params(struct snd_soc_component *component,
                                    struct snd_pcm_substream *substream,
                                    struct snd_pcm_hw_params *params)
{
    struct connector3_audio *priv = connector3_from(component);
    struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
    struct snd_soc_dai *cpu;
    int i, ret;

    /* After the SAIs' own hw_params, which set their FIFO burst. */
    for_each_rtd_cpu_dais(rtd, i, cpu) {
        struct dma_slave_config config = { };

        ret = snd_hwparams_to_dma_slave_config(substream, params, &config);
        if (ret)
            return ret;
        snd_dmaengine_pcm_set_config_from_dai_data(substream,
            snd_soc_dai_get_dma_data(cpu, substream), &config);
        ret = dmaengine_slave_config(priv->dma[i], &config);
        if (ret)
            return ret;
    }
    return 0;
}

static void connector3_pcm_period(void *data)
{
    snd_pcm_period_elapsed(data);
}

static int connector3_pcm_start(struct connector3_audio *priv, struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    int i;

    for (i = 0; i < CONNECTOR3_SAIS; i++) {
        struct dma_async_tx_descriptor *desc = dmaengine_prep_dma_cyclic(priv->dma[i],
            runtime->dma_addr, snd_pcm_lib_buffer_bytes(substream),
            snd_pcm_lib_period_bytes(substream), DMA_MEM_TO_DEV,
            i == 0 ? DMA_PREP_INTERRUPT | DMA_CTRL_ACK : DMA_CTRL_ACK);
        dma_cookie_t cookie;

        if (!desc)
            goto fail;
        if (i == 0) {
            desc->callback = connector3_pcm_period;
            desc->callback_param = substream;
        }
        cookie = dmaengine_submit(desc);
        if (dma_submit_error(cookie))
            goto fail;
        if (i == 0)
            priv->cookie = cookie;
    }
    for (i = 0; i < CONNECTOR3_SAIS; i++)
        dma_async_issue_pending(priv->dma[i]);
    return 0;
fail:
    for (i = 0; i < CONNECTOR3_SAIS; i++)
        dmaengine_terminate_async(priv->dma[i]);
    return -ENOMEM;
}

static int connector3_pcm_trigger(struct snd_soc_component *component,
                                  struct snd_pcm_substream *substream, int cmd)
{
    struct connector3_audio *priv = connector3_from(component);
    int i;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
    case SNDRV_PCM_TRIGGER_RESUME:
        return connector3_pcm_start(priv, substream);
    case SNDRV_PCM_TRIGGER_STOP:
    case SNDRV_PCM_TRIGGER_SUSPEND:
        for (i = 0; i < CONNECTOR3_SAIS; i++)
            dmaengine_terminate_async(priv->dma[i]);
        return 0;
    }
    return -EINVAL;
}

static snd_pcm_uframes_t connector3_pcm_pointer(struct snd_soc_component *component,
                                                struct snd_pcm_substream *substream)
{
    struct connector3_audio *priv = connector3_from(component);
    struct dma_tx_state state;
    unsigned int buffer = snd_pcm_lib_buffer_bytes(substream);
    unsigned int position = 0;

    if (dmaengine_tx_status(priv->dma[0], priv->cookie, &state) != DMA_ERROR &&
        state.residue && state.residue <= buffer)
        position = buffer - state.residue;
    return bytes_to_frames(substream->runtime, position);
}

static int connector3_pcm_sync_stop(struct snd_soc_component *component,
                                    struct snd_pcm_substream *substream)
{
    struct connector3_audio *priv = connector3_from(component);
    int i;

    for (i = 0; i < CONNECTOR3_SAIS; i++)
        dmaengine_synchronize(priv->dma[i]);
    return 0;
}

static int connector3_pcm_construct(struct snd_soc_component *component,
                                    struct snd_soc_pcm_runtime *rtd)
{
    struct snd_pcm_substream *substream =
        rtd->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;

    if (substream)
        snd_pcm_set_managed_buffer(substream, SNDRV_DMA_TYPE_DEV,
                                   connector3_from(component)->dma[0]->device->dev, 512 * 1024,
                                   connector3_pcm_hardware.buffer_bytes_max);
    return 0;
}

static const struct snd_soc_component_driver connector3_pcm_component = {
    .name = "galdrun-connector3-pcm",
    .open = connector3_pcm_open,
    .hw_params = connector3_pcm_hw_params,
    .trigger = connector3_pcm_trigger,
    .pointer = connector3_pcm_pointer,
    .sync_stop = connector3_pcm_sync_stop,
    .pcm_construct = connector3_pcm_construct,
};

static void connector3_audio_release(void *data)
{
    struct connector3_audio *priv = data;
    int i;

    if (priv->clocks_enabled)
        clk_bulk_disable_unprepare(ARRAY_SIZE(priv->clocks), priv->clocks);
    for (i = 0; i < CONNECTOR3_SAIS; i++) {
        if (!IS_ERR_OR_NULL(priv->dma[i]))
            dma_release_channel(priv->dma[i]);
        of_node_put(priv->cpus[i].of_node);
        of_node_put(priv->codecs[i].of_node);
    }
}

/* The SAI's TX channel, requested for this driver's PCM. The SAI's own
 * dmaengine component keeps its channel unused: it is not part of the link.
 * Straight from the SAI's device-tree node: dma_request_chan() on the SAI
 * device would add a second "dma:tx" sysfs link to it, which already exists
 * (a warning with a stack trace at every boot). */
static struct dma_chan *connector3_request_dma(struct device_node *sai)
{
    return of_dma_request_slave_channel(sai, "tx");
}

static int connector3_audio_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct connector3_audio *priv;
    int i, ret;

    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;
    mutex_init(&priv->lock);
    priv->clocks[0].id = "pll";
    priv->clocks[1].id = "mclk";
    priv->clocks[2].id = "sai";
    priv->clocks[3].id = "sai2";
    ret = devm_clk_bulk_get(dev, ARRAY_SIZE(priv->clocks), priv->clocks);
    if (ret)
        return dev_err_probe(dev, ret, "audio clocks unavailable\n");
    ret = clk_bulk_prepare_enable(ARRAY_SIZE(priv->clocks), priv->clocks);
    if (ret)
        return ret;
    priv->clocks_enabled = true;
    priv->nominal_pll = clk_get_rate(priv->clocks[0].clk);
    for (i = 0; i < CONNECTOR3_SAIS; i++) {
        priv->cpus[i].of_node = of_parse_phandle(dev->of_node, "audio-cpu", i);
        priv->codecs[i].of_node = of_parse_phandle(dev->of_node, "audio-codec", i);
    }
    ret = devm_add_action_or_reset(dev, connector3_audio_release, priv);
    if (ret)
        return ret;
    for (i = 0; i < CONNECTOR3_SAIS; i++) {
        if (!priv->cpus[i].of_node || !priv->codecs[i].of_node)
            return -EINVAL;
        priv->codecs[i].dai_name = "dit-hifi";
        priv->dma[i] = connector3_request_dma(priv->cpus[i].of_node);
        if (IS_ERR(priv->dma[i])) {
            ret = PTR_ERR(priv->dma[i]);
            priv->dma[i] = NULL;
            return dev_err_probe(dev, ret, "SAI%d TX DMA unavailable\n", i + 1);
        }
    }
    ret = devm_snd_soc_register_component(dev, &connector3_pcm_component, NULL, 0);
    if (ret)
        return ret;
    /* Both SAIs in one link, each with its S/PDIF-style dummy codec (1:1). */
    priv->platforms[0].of_node = dev->of_node;
    priv->links[0].name = "Connector3 I2S + S/PDIF";
    priv->links[0].stream_name = "Analog + Digital";
    priv->links[0].cpus = priv->cpus;
    priv->links[0].num_cpus = CONNECTOR3_SAIS;
    priv->links[0].codecs = priv->codecs;
    priv->links[0].num_codecs = CONNECTOR3_SAIS;
    priv->links[0].platforms = priv->platforms;
    priv->links[0].num_platforms = 1;
    priv->links[0].playback_only = 1;
    priv->links[0].dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
    priv->links[0].ops = &connector3_audio_ops;
    priv->card.owner = THIS_MODULE;
    priv->card.dev = dev;
    priv->card.name = "Connector3";
    priv->card.dai_link = priv->links;
    priv->card.num_links = CONNECTOR3_LINKS;
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
