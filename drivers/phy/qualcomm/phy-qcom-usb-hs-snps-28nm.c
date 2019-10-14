// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2009-2018, Linux Foundation. All rights reserved.
 * Copyright (c) 2018, Linaro Limited
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/extcon.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/ulpi/driver.h>

/* PHY register and bit definitions */
#define PHY_CTRL_COMMON0		0x078
#define SIDDQ				BIT(2)
#define PHY_IRQ_CMD			0x0d0
#define PHY_INTR_MASK0			0x0d4
#define PHY_INTR_CLEAR0			0x0dc
#define DPDM_MASK			0x1e
#define DP_1_0				BIT(4)
#define DP_0_1				BIT(3)
#define DM_1_0				BIT(2)
#define DM_0_1				BIT(1)

enum hsphy_voltage {
	VOL_NONE,
	VOL_MIN,
	VOL_MAX,
	VOL_NUM,
};

enum hsphy_vreg {
	VDD,
	VDDA_1P8,
	VDDA_3P3,
	VREG_NUM,
};

struct hsphy_init_seq {
	int offset;
	int val;
	int delay;
};

struct hsphy_data {
	const struct hsphy_init_seq *init_seq;
	unsigned int init_seq_num;
};

struct hsphy_priv {
	struct ulpi *ulpi;
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *phy_reset;
	struct reset_control *por_reset;
	struct regulator_bulk_data vregs[VREG_NUM];
	unsigned int voltages[VREG_NUM][VOL_NUM];
	const struct hsphy_data *data;
	bool cable_connected;
	bool cable_connected_at_power_on;
	struct extcon_dev *vbus_edev;
	struct notifier_block vbus_notify;
	enum phy_mode mode;
};

static int qcom_snps_hsphy_config_regulators(struct hsphy_priv *priv, int high)
{
	int old_uV[VREG_NUM];
	int min, ret, i;

	pr_err("%s\n", __func__);

	min = high ? 1 : 0; /* low or none? */

	for (i = 0; i < VREG_NUM; i++) {
		old_uV[i] = regulator_get_voltage(priv->vregs[i].consumer);
		ret = regulator_set_voltage(priv->vregs[i].consumer,
					    priv->voltages[i][min],
					    priv->voltages[i][VOL_MAX]);
		if (ret)
			goto roll_back;
	}

	return 0;

roll_back:
	for (; i >= 0; i--)
		regulator_set_voltage(priv->vregs[i].consumer,
				      old_uV[i], old_uV[i]);
	return ret;
}

static int qcom_snps_hsphy_enable_regulators(struct hsphy_priv *priv)
{
	int ret;

	pr_err("%s\n", __func__);

	ret = qcom_snps_hsphy_config_regulators(priv, 1);
	if (ret)
		return ret;

	ret = regulator_set_load(priv->vregs[VDDA_1P8].consumer, 19000);
	if (ret < 0)
		goto unconfig_regulators;

	ret = regulator_set_load(priv->vregs[VDDA_3P3].consumer, 16000);
	if (ret < 0)
		goto unset_1p8_load;

	ret = regulator_bulk_enable(VREG_NUM, priv->vregs);
	if (ret)
		goto unset_3p3_load;

	return 0;

unset_3p3_load:
	regulator_set_load(priv->vregs[VDDA_3P3].consumer, 0);
unset_1p8_load:
	regulator_set_load(priv->vregs[VDDA_1P8].consumer, 0);
unconfig_regulators:
	qcom_snps_hsphy_config_regulators(priv, 0);
	return ret;
}

static void qcom_snps_hsphy_disable_regulators(struct hsphy_priv *priv)
{
	pr_err("%s\n", __func__);

	regulator_bulk_disable(VREG_NUM, priv->vregs);
	regulator_set_load(priv->vregs[VDDA_1P8].consumer, 0);
	regulator_set_load(priv->vregs[VDDA_3P3].consumer, 0);
	qcom_snps_hsphy_config_regulators(priv, 0);
}

static int qcom_snps_hsphy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct hsphy_priv *priv = phy_get_drvdata(phy);

	dev_err(&phy->dev, "%s\n", __func__);

	priv->mode = mode;

	return 0;
}

static void qcom_snps_hsphy_enable_hv_interrupts(struct hsphy_priv *priv)
{
	u32 val;

	pr_err("%s\n", __func__);

	/* Clear any existing interrupts before enabling the interrupts */
	val = ulpi_read(priv->ulpi, PHY_INTR_CLEAR0);
	val |= DPDM_MASK;
	ulpi_write(priv->ulpi, PHY_INTR_CLEAR0, val);

	ulpi_write(priv->ulpi, PHY_IRQ_CMD, 0x0);
	usleep_range(200, 220);
	ulpi_write(priv->ulpi, PHY_IRQ_CMD, 0x1);

	/* Make sure the interrupts are cleared */
	usleep_range(200, 220);

	val = ulpi_read(priv->ulpi, PHY_INTR_MASK0);
	switch (priv->mode) {
	case PHY_MODE_USB_HOST_HS:
	case PHY_MODE_USB_HOST_FS:
	case PHY_MODE_USB_DEVICE_HS:
	case PHY_MODE_USB_DEVICE_FS:
		val |= DP_1_0 | DM_0_1;
		break;
	case PHY_MODE_USB_HOST_LS:
	case PHY_MODE_USB_DEVICE_LS:
		val |= DP_0_1 | DM_1_0;
		break;
	default:
		/* No device connected */
		val |= DP_0_1 | DM_0_1;
		break;
	}
	ulpi_write(priv->ulpi, PHY_INTR_MASK0, val);
}

static void qcom_snps_hsphy_disable_hv_interrupts(struct hsphy_priv *priv)
{
	u32 val;

	pr_err("%s\n", __func__);

	val = ulpi_read(priv->ulpi, PHY_INTR_MASK0);
	val &= ~DPDM_MASK;
	ulpi_write(priv->ulpi, PHY_INTR_MASK0, val);

	/* Clear any pending interrupts */
	val = ulpi_read(priv->ulpi, PHY_INTR_CLEAR0);
	val |= DPDM_MASK;
	ulpi_write(priv->ulpi, PHY_INTR_CLEAR0, val);

	ulpi_write(priv->ulpi, PHY_IRQ_CMD, 0x0);
	usleep_range(200, 220);

	ulpi_write(priv->ulpi, PHY_IRQ_CMD, 0x1);
	usleep_range(200, 220);
}

static void qcom_snps_hsphy_enter_retention(struct hsphy_priv *priv)
{
	u32 val;

	pr_err("%s\n", __func__);

	val = ulpi_read(priv->ulpi, PHY_CTRL_COMMON0);

	val |= SIDDQ;
	ulpi_write(priv->ulpi, PHY_CTRL_COMMON0, val);
}

static void qcom_snps_hsphy_exit_retention(struct hsphy_priv *priv)
{
	u32 val;

	pr_err("%s\n", __func__);

	val = ulpi_read(priv->ulpi, PHY_CTRL_COMMON0);
	val &= ~SIDDQ;
	ulpi_write(priv->ulpi, PHY_CTRL_COMMON0, val);
}

static int qcom_snps_hsphy_vbus_notifier(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct hsphy_priv *priv = container_of(nb, struct hsphy_priv,
						    vbus_notify);
	pr_err("%s\n", __func__);
	priv->cable_connected = !!event;
	return 0;
}

static int qcom_snps_hsphy_power_on(struct phy *phy)
{
	struct hsphy_priv *priv = phy_get_drvdata(phy);
	int ret;
	dev_err(&phy->dev, "%s\n", __func__);

	priv->cable_connected_at_power_on = priv->cable_connected;

	if (priv->cable_connected_at_power_on) {
		ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
		if (ret)
			return ret;
		qcom_snps_hsphy_disable_hv_interrupts(priv);
	} else {
		ret = qcom_snps_hsphy_enable_regulators(priv);
		if (ret)
			return ret;
		ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
		if (ret)
			return ret;
		qcom_snps_hsphy_exit_retention(priv);
	}

	return 0;
}

static int qcom_snps_hsphy_power_off(struct phy *phy)
{
	struct hsphy_priv *priv = phy_get_drvdata(phy);
	dev_err(&phy->dev, "%s\n", __func__);

	if (priv->cable_connected_at_power_on) {
		qcom_snps_hsphy_enable_hv_interrupts(priv);
		clk_bulk_disable_unprepare(priv->num_clks, priv->clks);
	} else {
		qcom_snps_hsphy_enter_retention(priv);
		clk_bulk_disable_unprepare(priv->num_clks, priv->clks);
		qcom_snps_hsphy_disable_regulators(priv);
	}

	return 0;
}

static int qcom_snps_hsphy_reset(struct hsphy_priv *priv)
{
	int ret;
	pr_err("%s\n", __func__);

	ret = reset_control_assert(priv->phy_reset);
	if (ret)
		return ret;

	usleep_range(10, 15);

	ret = reset_control_deassert(priv->phy_reset);
	if (ret)
		return ret;

	usleep_range(80, 100);

	return 0;
}

static void qcom_snps_hsphy_init_sequence(struct hsphy_priv *priv)
{
	const struct hsphy_data *data = priv->data;
	const struct hsphy_init_seq *seq;
	int i;
	pr_err("%s\n", __func__);

	/* Device match data is optional. */
	if (!data)
		return;

	seq = data->init_seq;

	for (i = 0; i < data->init_seq_num; i++, seq++) {
		ulpi_write(priv->ulpi, seq->offset, seq->val);
		if (seq->delay)
			usleep_range(seq->delay, seq->delay + 10);
	}
}

static int qcom_snps_hsphy_por_reset(struct hsphy_priv *priv)
{
	int ret;

	pr_err("%s\n", __func__);

	ret = reset_control_assert(priv->por_reset);
	if (ret)
		return ret;

	/*
	 * The Femto PHY is POR reset in the following scenarios.
	 *
	 * 1. After overriding the parameter registers.
	 * 2. Low power mode exit from PHY retention.
	 *
	 * Ensure that SIDDQ is cleared before bringing the PHY
	 * out of reset.
	 */
	qcom_snps_hsphy_exit_retention(priv);

	/*
	 * As per databook, 10 usec delay is required between
	 * PHY POR assert and de-assert.
	 */
	usleep_range(10, 20);
	ret = reset_control_deassert(priv->por_reset);
	if (ret)
		return ret;

	/*
	 * As per databook, it takes 75 usec for PHY to stabilize
	 * after the reset.
	 */
	usleep_range(80, 100);

	return 0;
}

static int qcom_snps_hsphy_init(struct phy *phy)
{
	struct hsphy_priv *priv = phy_get_drvdata(phy);
	int state;
	int ret;

	dev_err(&phy->dev, "%s\n", __func__);

	ret = qcom_snps_hsphy_reset(priv);
	if (ret)
		return ret;

	qcom_snps_hsphy_init_sequence(priv);

	ret = qcom_snps_hsphy_por_reset(priv);
	if (ret)
		return ret;

	/* Setup initial state */
	if (priv->vbus_edev) {
		state = extcon_get_state(priv->vbus_edev, EXTCON_USB);
		ret = qcom_snps_hsphy_vbus_notifier(&priv->vbus_notify, state,
						    priv->vbus_edev);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct phy_ops qcom_snps_hsphy_ops = {
	.init = qcom_snps_hsphy_init,
	.power_on = qcom_snps_hsphy_power_on,
	.power_off = qcom_snps_hsphy_power_off,
	.set_mode = qcom_snps_hsphy_set_mode,
	.owner = THIS_MODULE,
};

static const char * const qcom_snps_hsphy_clks[] = {
	"ref",
	"phy",
	"sleep",
};

static int qcom_snps_hsphy_probe(struct ulpi *ulpi)
{
	struct device *dev = &ulpi->dev;
	struct device_node *extcon_node;
	struct phy_provider *provider;
	struct hsphy_priv *priv;
	struct phy *phy;
	int ret;
	int i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ulpi = ulpi;

	priv->num_clks = ARRAY_SIZE(qcom_snps_hsphy_clks);
	priv->clks = devm_kcalloc(dev, priv->num_clks, sizeof(*priv->clks),
				  GFP_KERNEL);
	if (!priv->clks)
		return -ENOMEM;

	for (i = 0; i < priv->num_clks; i++)
		priv->clks[i].id = qcom_snps_hsphy_clks[i];

	ret = devm_clk_bulk_get(dev, priv->num_clks, priv->clks);
	if (ret)
		return ret;

	priv->phy_reset = devm_reset_control_get(dev, "phy");
	if (IS_ERR(priv->phy_reset))
		return PTR_ERR(priv->phy_reset);

	priv->por_reset = devm_reset_control_get(dev, "por");
	if (IS_ERR(priv->por_reset))
		return PTR_ERR(priv->por_reset);

	priv->vregs[VDD].supply = "vdd";
	priv->vregs[VDDA_1P8].supply = "vdda1p8";
	priv->vregs[VDDA_3P3].supply = "vdda3p3";

	ret = devm_regulator_bulk_get(dev, VREG_NUM, priv->vregs);
	dev_err(dev, "reg bulk get: %d\n", ret);
	if (ret)
		return ret;

	priv->voltages[VDDA_1P8][VOL_NONE] = 0;
	priv->voltages[VDDA_1P8][VOL_MIN] = 1800000;
	priv->voltages[VDDA_1P8][VOL_MAX] = 1800000;

	priv->voltages[VDDA_3P3][VOL_NONE] = 0;
	priv->voltages[VDDA_3P3][VOL_MIN] = 3050000;
	priv->voltages[VDDA_3P3][VOL_MAX] = 3300000;

	ret = of_property_read_u32_array(dev->of_node, "qcom,vdd-voltage-level",
					 priv->voltages[VDD], VOL_NUM);
	if (ret) {
		dev_err(dev, "failed to read qcom,vdd-voltage-level\n");
		return ret;
	}
	if (device_property_read_bool(dev, "extcon"))
		priv->vbus_edev = extcon_get_edev_by_phandle(dev, 0);

	if (IS_ERR(priv->vbus_edev)) {
		dev_err(dev, "Failed to get extcon by phandle. Trying remote node");

		extcon_node = of_graph_get_remote_node(dev->of_node, -1, -1);
		dev_err(dev, "Extcon node is %px\n", extcon_node);
		if (extcon_node) {
			dev_err(dev, "Extcon node name: %s\n", extcon_node->name);
			priv->vbus_edev = extcon_find_edev_by_node(extcon_node);
			dev_err(dev, "vbus_edev is %px\n", priv->vbus_edev);
			if (IS_ERR(priv->vbus_edev)) {
				dev_err(dev, "vbus_edev ptr_err is %ld", PTR_ERR(priv->vbus_edev));
				if (PTR_ERR(priv->vbus_edev) != -ENODEV) {
					of_node_put(extcon_node);
					return PTR_ERR(priv->vbus_edev);
				}
				priv->vbus_edev = NULL;
			}
		} else {
			priv->vbus_edev = NULL;
		}
		of_node_put(extcon_node);
	}

	if (priv->vbus_edev) {
		dev_err(dev, "Registering extcon notifier\n");
		priv->vbus_notify.notifier_call = qcom_snps_hsphy_vbus_notifier;
		ret = devm_extcon_register_notifier(dev, priv->vbus_edev,
						    EXTCON_USB,
						    &priv->vbus_notify);
		if (ret)
			return ret;
	}

	/* Get device match data */
	priv->data = device_get_match_data(dev);

	phy = devm_phy_create(dev, dev->of_node, &qcom_snps_hsphy_ops);
	dev_err(dev, "phy create: %px\n", phy);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	phy_set_drvdata(phy, priv);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	dev_err(dev, "Registered as %px %ld\n", provider, PTR_ERR_OR_ZERO(provider));
	return PTR_ERR_OR_ZERO(provider);
}

/*
 * The macro is used to define an initialization sequence.  Each tuple
 * is meant to program 'value' into phy register at 'offset' with 'delay'
 * in us followed.
 */
#define HSPHY_INIT_CFG(o, v, d)	{ .offset = o, .val = v, .delay = d, }

static const struct hsphy_init_seq init_seq_msm8956[] = {
	HSPHY_INIT_CFG(0x80, 0x73, 0),
	HSPHY_INIT_CFG(0x81, 0x38, 0),
	HSPHY_INIT_CFG(0x82, 0x1b, 0),
};

static const struct hsphy_data hsphy_data_msm8956 = {
	.init_seq = init_seq_msm8956,
	.init_seq_num = ARRAY_SIZE(init_seq_msm8956),
};

static const struct hsphy_init_seq init_seq_msm8956_suzu[] = {
	HSPHY_INIT_CFG(0x80, 0x74, 0),
	HSPHY_INIT_CFG(0x81, 0x6f, 0),
	HSPHY_INIT_CFG(0x82, 0x3f, 0),
	HSPHY_INIT_CFG(0x83, 0x33, 0),
};

static const struct hsphy_data hsphy_data_msm8956_suzu = {
	.init_seq = init_seq_msm8956_suzu,
	.init_seq_num = ARRAY_SIZE(init_seq_msm8956_suzu),
};

static const struct hsphy_init_seq init_seq_msm8956_kugo[] = {
	HSPHY_INIT_CFG(0x80, 0x74, 0),
	HSPHY_INIT_CFG(0x81, 0x68, 0),
	HSPHY_INIT_CFG(0x82, 0x3f, 0),
	HSPHY_INIT_CFG(0x83, 0x33, 0),
};

static const struct hsphy_data hsphy_data_msm8956_kugo = {
	.init_seq = init_seq_msm8956_kugo,
	.init_seq_num = ARRAY_SIZE(init_seq_msm8956_kugo),
};

static const struct hsphy_init_seq init_seq_qcs404[] = {
	HSPHY_INIT_CFG(0xc0, 0x01, 0),
	HSPHY_INIT_CFG(0xe8, 0x0d, 0),
	HSPHY_INIT_CFG(0x74, 0x12, 0),
	HSPHY_INIT_CFG(0x98, 0x63, 0),
	HSPHY_INIT_CFG(0x9c, 0x03, 0),
	HSPHY_INIT_CFG(0xa0, 0x1d, 0),
	HSPHY_INIT_CFG(0xa4, 0x03, 0),
	HSPHY_INIT_CFG(0x8c, 0x23, 0),
	HSPHY_INIT_CFG(0x78, 0x08, 0),
	HSPHY_INIT_CFG(0x7c, 0xdc, 0),
	HSPHY_INIT_CFG(0x90, 0xe0, 20),
	HSPHY_INIT_CFG(0x74, 0x10, 0),
	HSPHY_INIT_CFG(0x90, 0x60, 0),
};

static const struct hsphy_data hsphy_data_qcs404 = {
	.init_seq = init_seq_qcs404,
	.init_seq_num = ARRAY_SIZE(init_seq_qcs404),
};

static const struct of_device_id qcom_snps_hsphy_match[] = {
	{ .compatible = "qcom,msm8956-suzu-usb-hsphy", .data = &hsphy_data_msm8956_suzu, },
	{ .compatible = "qcom,msm8956-kugo-usb-hsphy", .data = &hsphy_data_msm8956_kugo, },
	{ .compatible = "qcom,qcs404-usb-hsphy", .data = &hsphy_data_qcs404, },
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_snps_hsphy_match);

static struct ulpi_driver qcom_snps_hsphy_driver = {
	.probe = qcom_snps_hsphy_probe,
	.driver	= {
		.name = "qcom-usb-snps-hsphy",
		.of_match_table = qcom_snps_hsphy_match,
	},
};
module_ulpi_driver(qcom_snps_hsphy_driver);

MODULE_DESCRIPTION("Qualcomm Synopsys 28nm USB High-Speed PHY driver");
MODULE_LICENSE("GPL v2");
