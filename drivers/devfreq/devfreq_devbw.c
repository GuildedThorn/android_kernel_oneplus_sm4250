// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2014, 2018, 2019, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "devbw: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/devfreq.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <trace/events/power.h>
#include <linux/msm-bus.h>
#include <linux/msm-bus-board.h>

/* Has to be ULL to prevent overflow where this macro is used. */
#define MBYTE (1ULL << 20)
#define MAX_PATHS	2
#define DBL_BUF		2

#ifdef OEM_TARGET_PRODUCT_BILLIE2
#include <linux/pm_qos.h>
struct qos_request_v {
	int max_state;
	int max_devfreq;
	int min_devfreq;
};

static bool cpubw_flag;
static struct qos_request_v qos_request_value = {
	.max_state = 0,
	.max_devfreq = INT_MAX,
	.min_devfreq = 0,
};
#endif

struct dev_data {
	struct msm_bus_vectors vectors[MAX_PATHS * DBL_BUF];
	struct msm_bus_paths bw_levels[DBL_BUF];
	struct msm_bus_scale_pdata bw_data;
	int num_paths;
	u32 bus_client;
	int cur_idx;
	int cur_ab;
	int cur_ib;
	long gov_ab;
	struct devfreq *df;
	struct devfreq_dev_profile dp;
};

static int set_bw(struct device *dev, int new_ib, int new_ab)
{
	struct dev_data *d = dev_get_drvdata(dev);
	int i, ret;

	if (d->cur_ib == new_ib && d->cur_ab == new_ab)
		return 0;

	i = (d->cur_idx + 1) % DBL_BUF;

	d->bw_levels[i].vectors[0].ib = new_ib * MBYTE;
	d->bw_levels[i].vectors[0].ab = new_ab / d->num_paths * MBYTE;
	d->bw_levels[i].vectors[1].ib = new_ib * MBYTE;
	d->bw_levels[i].vectors[1].ab = new_ab / d->num_paths * MBYTE;

	dev_dbg(dev, "BW MBps: AB: %d IB: %d\n", new_ab, new_ib);

	ret = msm_bus_scale_client_update_request(d->bus_client, i);
	if (ret) {
		dev_err(dev, "bandwidth request failed (%d)\n", ret);
	} else {
		d->cur_idx = i;
		d->cur_ib = new_ib;
		d->cur_ab = new_ab;
	}

	return ret;
}

#ifdef OEM_TARGET_PRODUCT_BILLIE2
static void find_freq_cpubw(struct devfreq_dev_profile *p, unsigned long *freq,
		u32 flags)
{
	int i;
	unsigned long atmost, atleast, f;
	int min_index, max_index;

	min_index = qos_request_value.min_devfreq;
	if (p->max_state > qos_request_value.max_devfreq)
		max_index = qos_request_value.max_devfreq;
	else
		max_index = p->max_state;

	atmost = p->freq_table[min_index];
	atleast = p->freq_table[max_index-1];

	for (i = min_index; i < max_index; i++) {
		f = p->freq_table[i];
		if (f <= *freq)
			atmost = max(f, atmost);
		if (f >= *freq)
			atleast = min(f, atleast);
	}

	if (flags & DEVFREQ_FLAG_LEAST_UPPER_BOUND)
		*freq = atmost;
	else
		*freq = atleast;
}

static int devbw_target_cpubw(struct device *dev, unsigned long *freq,
		u32 flags)
{
	struct dev_data *d = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (!IS_ERR(opp))
		dev_pm_opp_put(opp);
	find_freq_cpubw(&d->dp, freq, flags);

	return set_bw(dev, *freq, d->gov_ab);
}
#endif

static int devbw_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct dev_data *d = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (!IS_ERR(opp))
		dev_pm_opp_put(opp);

	return set_bw(dev, *freq, d->gov_ab);
}

static int devbw_get_dev_status(struct device *dev,
				struct devfreq_dev_status *stat)
{
	struct dev_data *d = dev_get_drvdata(dev);

	stat->private_data = &d->gov_ab;
	return 0;
}

#ifdef OEM_TARGET_PRODUCT_BILLIE2
static int devfreq_qos_handler(struct notifier_block *b, unsigned long val,
		void *v)
{
	unsigned int max_devfreq_index, min_devfreq_index;
	unsigned int index_max = 0, index_min = 0;

	max_devfreq_index = (unsigned int)pm_qos_request(PM_QOS_DEVFREQ_MAX);
	min_devfreq_index = (unsigned int)pm_qos_request(PM_QOS_DEVFREQ_MIN);

	/* add limit */
	if (max_devfreq_index & MASK_CPUFREQ) {
		index_max = MAX_CPUFREQ - max_devfreq_index;
		if (index_max > qos_request_value.max_state)
			index_max = 0;
		index_max = qos_request_value.max_state - index_max;
	} else {
		if (max_devfreq_index > qos_request_value.max_state)
			index_max = qos_request_value.max_state;
	}
	if (min_devfreq_index & MASK_CPUFREQ) {
		index_min = MAX_CPUFREQ - min_devfreq_index;
		if (index_min > (qos_request_value.max_state-1))
			index_min = 0;
		index_min = qos_request_value.max_state - 1 - index_min;
	} else {
		if (min_devfreq_index > qos_request_value.max_state)
			index_min = qos_request_value.max_state - 1;
	}

	qos_request_value.min_devfreq = index_min;
	qos_request_value.max_devfreq = index_max;

	return NOTIFY_OK;
}
static struct notifier_block devfreq_qos_notifier = {
	.notifier_call = devfreq_qos_handler,
};
#endif

#define PROP_PORTS "qcom,src-dst-ports"
#define PROP_ACTIVE "qcom,active-only"

int devfreq_add_devbw(struct device *dev)
{
	struct dev_data *d;
	struct devfreq_dev_profile *p;
	u32 ports[MAX_PATHS * 2];
	const char *gov_name;
	int ret, len, i, num_paths;
	struct opp_table *opp_table;
	u32 version;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	dev_set_drvdata(dev, d);

	if (of_find_property(dev->of_node, PROP_PORTS, &len)) {
		len /= sizeof(ports[0]);
		if (len % 2 || len > ARRAY_SIZE(ports)) {
			dev_err(dev, "Unexpected number of ports\n");
			return -EINVAL;
		}

		ret = of_property_read_u32_array(dev->of_node, PROP_PORTS,
						 ports, len);
		if (ret)
			return ret;

		num_paths = len / 2;
	} else {
		return -EINVAL;
	}

	d->bw_levels[0].vectors = &d->vectors[0];
	d->bw_levels[1].vectors = &d->vectors[MAX_PATHS];
	d->bw_data.usecase = d->bw_levels;
	d->bw_data.num_usecases = ARRAY_SIZE(d->bw_levels);
	d->bw_data.name = dev_name(dev);
	d->bw_data.active_only = of_property_read_bool(dev->of_node,
							PROP_ACTIVE);

	for (i = 0; i < num_paths; i++) {
		d->bw_levels[0].vectors[i].src = ports[2 * i];
		d->bw_levels[0].vectors[i].dst = ports[2 * i + 1];
		d->bw_levels[1].vectors[i].src = ports[2 * i];
		d->bw_levels[1].vectors[i].dst = ports[2 * i + 1];
	}
	d->bw_levels[0].num_paths = num_paths;
	d->bw_levels[1].num_paths = num_paths;
	d->num_paths = num_paths;

	p = &d->dp;
	p->polling_ms = 50;
#ifdef OEM_TARGET_PRODUCT_BILLIE2
	if (strnstr(d->bw_data.name, "soc:qcom,cpu-cpu-llcc-bw",
				strlen(d->bw_data.name)) != NULL) {
		p->target = devbw_target_cpubw;
		cpubw_flag = true;
	} else
		p->target = devbw_target;
#else
	p->target = devbw_target;
#endif

	p->get_dev_status = devbw_get_dev_status;

	if (of_device_is_compatible(dev->of_node, "qcom,devbw-ddr")) {
		version = (1 << of_fdt_get_ddrtype());
		opp_table = dev_pm_opp_set_supported_hw(dev, &version, 1);
		if (IS_ERR(opp_table)) {
			dev_err(dev, "Failed to set supported hardware\n");
			return PTR_ERR(opp_table);
		}
	}

	ret = dev_pm_opp_of_add_table(dev);
	if (ret)
		dev_err(dev, "Couldn't parse OPP table:%d\n", ret);

	d->bus_client = msm_bus_scale_register_client(&d->bw_data);
	if (!d->bus_client) {
		dev_err(dev, "Unable to register bus client\n");
		return -ENODEV;
	}

	if (of_property_read_string(dev->of_node, "governor", &gov_name))
		gov_name = "performance";

	d->df = devfreq_add_device(dev, p, gov_name, NULL);
	if (IS_ERR(d->df)) {
		msm_bus_scale_unregister_client(d->bus_client);
		return PTR_ERR(d->df);
	}

#ifdef OEM_TARGET_PRODUCT_BILLIE2
	if (cpubw_flag) {
		cpubw_flag = false;
		qos_request_value.max_state = p->max_state;
		qos_request_value.min_devfreq = 0;
		qos_request_value.max_devfreq = p->max_state;
	}
#endif
	return 0;
}

int devfreq_remove_devbw(struct device *dev)
{
	struct dev_data *d = dev_get_drvdata(dev);

	msm_bus_scale_unregister_client(d->bus_client);
	devfreq_remove_device(d->df);
	return 0;
}

int devfreq_suspend_devbw(struct device *dev)
{
	struct dev_data *d = dev_get_drvdata(dev);

	return devfreq_suspend_device(d->df);
}

int devfreq_resume_devbw(struct device *dev)
{
	struct dev_data *d = dev_get_drvdata(dev);

	return devfreq_resume_device(d->df);
}

static int devfreq_devbw_probe(struct platform_device *pdev)
{
	return devfreq_add_devbw(&pdev->dev);
}

static int devfreq_devbw_remove(struct platform_device *pdev)
{
	return devfreq_remove_devbw(&pdev->dev);
}

static const struct of_device_id devbw_match_table[] = {
	{ .compatible = "qcom,devbw-llcc" },
	{ .compatible = "qcom,devbw-ddr" },
	{ .compatible = "qcom,devbw" },
	{}
};

static struct platform_driver devbw_driver = {
	.probe = devfreq_devbw_probe,
	.remove = devfreq_devbw_remove,
	.driver = {
		.name = "devbw",
		.of_match_table = devbw_match_table,
#ifdef OEM_TARGET_PRODUCT_BILLIE2
		.owner = THIS_MODULE,
#endif
		.suppress_bind_attrs = true,
	},
};

#ifdef OEM_TARGET_PRODUCT_BILLIE2
static int __init devbw_init(void)
{
	/* add cpufreq qos notify */
	cpubw_flag = false;
	pm_qos_add_notifier(PM_QOS_DEVFREQ_MAX, &devfreq_qos_notifier);
	pm_qos_add_notifier(PM_QOS_DEVFREQ_MIN, &devfreq_qos_notifier);
	platform_driver_register(&devbw_driver);
	return 0;
}
device_initcall(devbw_init);
#endif

module_platform_driver(devbw_driver);
MODULE_DESCRIPTION("Device DDR bandwidth voting driver MSM SoCs");
MODULE_LICENSE("GPL v2");
