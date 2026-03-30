// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal out-of-tree Rockchip HDMI machine driver for HDMI RX audio testing.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#define DRV_NAME "rockchip-hdmirx-sound-test"
#define MAX_CODECS 2
#define DEFAULT_MCLK_FS 256

struct rk_hdmi_test_data {
	struct snd_soc_card card;
	struct snd_soc_dai_link dai;
	struct snd_soc_jack hdmi_jack;
	struct snd_soc_jack_pin hdmi_jack_pin;
	unsigned int mclk_fs;
	bool jack_det;
};

static int rk_hdmi_test_fill_widget_info(struct device *dev,
					 struct snd_soc_dapm_widget *w,
					 enum snd_soc_dapm_type id,
					 void *priv, const char *wname,
					 const char *stream,
					 struct snd_kcontrol_new *wc,
					 int numkc,
					 int (*event)(struct snd_soc_dapm_widget *,
						      struct snd_kcontrol *, int),
					 unsigned short event_flags)
{
	w->id = id;
	w->name = devm_kstrdup(dev, wname, GFP_KERNEL);
	if (!w->name)
		return -ENOMEM;

	w->sname = stream;
	w->reg = SND_SOC_NOPM;
	w->shift = 0;
	w->kcontrol_news = wc;
	w->num_kcontrols = numkc;
	w->priv = priv;
	w->event = event;
	w->event_flags = event_flags;

	return 0;
}

static int rk_hdmi_test_dailink_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct rk_hdmi_test_data *rk_data = snd_soc_card_get_drvdata(card);
	struct snd_soc_dapm_widget *widgets;
	int ret;

	if (!rk_data->jack_det)
		return 0;

	widgets = devm_kcalloc(card->dev, 1, sizeof(*widgets), GFP_KERNEL);
	if (!widgets)
		return -ENOMEM;

	ret = rk_hdmi_test_fill_widget_info(card->dev, widgets,
					    snd_soc_dapm_line, NULL,
					    rk_data->hdmi_jack_pin.pin,
					    NULL, NULL, 0, NULL, 0);
	if (ret < 0)
		return ret;

	ret = snd_soc_dapm_new_controls(card->dapm, widgets, 1);
	if (ret < 0)
		return ret;

	ret = snd_soc_dapm_new_widgets(card);
	if (ret < 0)
		return ret;

	ret = snd_soc_card_jack_new_pins(card, rk_data->hdmi_jack_pin.pin,
					 rk_data->hdmi_jack_pin.mask,
					 &rk_data->hdmi_jack,
					 &rk_data->hdmi_jack_pin, 1);
	if (ret)
		return ret;

	return snd_soc_component_set_jack(codec_dai->component,
					  &rk_data->hdmi_jack, NULL);
}

static int rk_hdmi_test_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct rk_hdmi_test_data *rk_data = snd_soc_card_get_drvdata(rtd->card);
	struct clk *mclk;
	const char *clk_name;
	unsigned int mclk_rate = params_rate(params) * rk_data->mclk_fs;
	int ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, substream->stream, mclk_rate,
				     SND_SOC_CLOCK_IN);
	if (ret && ret != -ENOTSUPP)
		return ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, substream->stream, mclk_rate,
				     SND_SOC_CLOCK_OUT);
	if (ret && ret != -ENOTSUPP)
		return ret;

	clk_name = substream->stream == SNDRV_PCM_STREAM_CAPTURE ?
		"mclk_rx" : "mclk_tx";
	mclk = clk_get(cpu_dai->dev, clk_name);
	if (!IS_ERR(mclk)) {
		ret = clk_set_rate(mclk, mclk_rate);
		clk_put(mclk);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct snd_soc_ops rk_hdmi_test_ops = {
	.hw_params = rk_hdmi_test_hw_params,
};

static unsigned int rk_hdmi_test_parse_daifmt(struct device_node *node,
					      struct device_node *codec,
					      const char *prefix)
{
	struct device_node *bitclkmaster = NULL;
	struct device_node *framemaster = NULL;
	unsigned int daifmt;

	daifmt = snd_soc_daifmt_parse_format(node, prefix);
	snd_soc_daifmt_parse_clock_provider_as_phandle(node, prefix,
						       &bitclkmaster,
						       &framemaster);
	if (!bitclkmaster && !framemaster) {
		daifmt |= snd_soc_daifmt_parse_clock_provider_as_flag(codec, NULL);
	} else {
		daifmt |= snd_soc_daifmt_clock_provider_from_bitmap(
			((codec == bitclkmaster) << 4) | (codec == framemaster));
	}

	if (!(daifmt & SND_SOC_DAIFMT_FORMAT_MASK))
		daifmt |= SND_SOC_DAIFMT_I2S;

	of_node_put(bitclkmaster);
	of_node_put(framemaster);

	return daifmt;
}

static int rk_hdmi_test_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct snd_soc_dai_link_component *codecs;
	struct snd_soc_dai_link_component *platforms;
	struct snd_soc_dai_link_component *cpus;
	struct of_phandle_args args;
	struct device_node *cpu_np;
	struct rk_hdmi_test_data *rk_data;
	u32 val;
	int count, ret, i, idx;

	rk_data = devm_kzalloc(&pdev->dev, sizeof(*rk_data), GFP_KERNEL);
	if (!rk_data)
		return -ENOMEM;

	cpus = devm_kzalloc(&pdev->dev, sizeof(*cpus), GFP_KERNEL);
	platforms = devm_kzalloc(&pdev->dev, sizeof(*platforms), GFP_KERNEL);
	if (!cpus || !platforms)
		return -ENOMEM;

	rk_data->card.dev = &pdev->dev;
	rk_data->dai.init = rk_hdmi_test_dailink_init;
	rk_data->dai.ops = &rk_hdmi_test_ops;
	rk_data->dai.cpus = cpus;
	rk_data->dai.platforms = platforms;
	rk_data->dai.num_cpus = 1;
	rk_data->dai.num_platforms = 1;

	ret = snd_soc_of_parse_card_name(&rk_data->card, "rockchip,card-name");
	if (ret < 0)
		return ret;

	rk_data->dai.name = rk_data->card.name;
	rk_data->dai.stream_name = rk_data->card.name;

	count = of_count_phandle_with_args(np, "rockchip,codec",
					   "#sound-dai-cells");
	if (count < 0 || count > MAX_CODECS)
		return -EINVAL;

	for (i = 0, idx = 0; i < count; i++) {
		ret = of_parse_phandle_with_args(np, "rockchip,codec",
						 "#sound-dai-cells", i, &args);
		if (ret)
			return -ENODEV;
		if (of_device_is_available(args.np))
			idx++;
		of_node_put(args.np);
	}

	if (!idx)
		return -ENODEV;

	codecs = devm_kcalloc(&pdev->dev, idx, sizeof(*codecs), GFP_KERNEL);
	if (!codecs)
		return -ENOMEM;

	rk_data->dai.codecs = codecs;
	rk_data->dai.num_codecs = idx;

	for (i = 0, idx = 0; i < count; i++) {
		ret = of_parse_phandle_with_args(np, "rockchip,codec",
						 "#sound-dai-cells", i, &args);
		if (ret)
			return -ENODEV;
		if (!of_device_is_available(args.np)) {
			of_node_put(args.np);
			continue;
		}
		codecs[idx].of_node = args.np;
		ret = snd_soc_get_dai_name(&args, &codecs[idx].dai_name);
		if (ret)
			return ret;
		idx++;
	}

	cpu_np = of_parse_phandle(np, "rockchip,cpu", 0);
	if (!cpu_np)
		return -ENODEV;

	rk_data->dai.dai_fmt = rk_hdmi_test_parse_daifmt(np,
							 codecs[0].of_node,
							 "rockchip,");
	rk_data->mclk_fs = DEFAULT_MCLK_FS;
	if (!of_property_read_u32(np, "rockchip,mclk-fs", &val))
		rk_data->mclk_fs = val;

	rk_data->jack_det = of_property_read_bool(np, "rockchip,jack-det");
	rk_data->dai.cpus->of_node = cpu_np;
	rk_data->dai.platforms->of_node = cpu_np;
	of_node_put(cpu_np);

	rk_data->hdmi_jack_pin.pin = rk_data->card.name;
	rk_data->hdmi_jack_pin.mask = SND_JACK_LINEOUT;
	rk_data->card.num_links = 1;
	rk_data->card.owner = THIS_MODULE;
	rk_data->card.dai_link = &rk_data->dai;

	snd_soc_card_set_drvdata(&rk_data->card, rk_data);

	return devm_snd_soc_register_card(&pdev->dev, &rk_data->card);
}

static const struct of_device_id rk_hdmi_test_of_match[] = {
	{ .compatible = "rockchip,hdmi" },
	{},
};
MODULE_DEVICE_TABLE(of, rk_hdmi_test_of_match);

static struct platform_driver rk_hdmi_test_driver = {
	.probe = rk_hdmi_test_probe,
	.driver = {
		.name = DRV_NAME,
		.pm = &snd_soc_pm_ops,
		.of_match_table = rk_hdmi_test_of_match,
	},
};
module_platform_driver(rk_hdmi_test_driver);

MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Rockchip HDMI RX sound test machine driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
