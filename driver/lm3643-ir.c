// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal driver for the TI LM3643 LED driver used as IR flood illuminator
 * next to the Himax HM1092 IR camera (Dell XPS 16 9640, ACPI \_SB.PC00.FLM0,
 * HID TXNW3643).  Exposes one LED class device that drives both LED outputs
 * in torch (continuous) mode.  brightness = torch code (0 = off),
 * I = code * 1.4 mA + 0.977 mA (datasheet SNVS967A, 9.6.5).
 */
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define LM3643_REG_ENABLE	0x01
#define LM3643_REG_LED1_TORCH	0x05
#define LM3643_REG_FLAGS1	0x0a
#define LM3643_REG_FLAGS2	0x0b
#define LM3643_REG_DEVICE_ID	0x0c

#define LM3643_ENABLE_LED1	BIT(0)
#define LM3643_ENABLE_LED2	BIT(1)
#define LM3643_MODE_TORCH	(2 << 2)
#define LM3643_TORCH_OVERRIDE	BIT(7)	/* LED2 torch current = LED1 */
#define LM3643_FLAGS1_TX	BIT(7)	/* TX pin event, irrelevant for torch */
#define LM3643_ID_MASK		GENMASK(5, 3)

static unsigned int max_code = 63;
module_param(max_code, uint, 0444);
MODULE_PARM_DESC(max_code,
	"Maximum torch code 1-127 (I = code*1.4 mA + 0.977 mA). Default 63 = 89 mA");

struct lm3643 {
	struct i2c_client *client;
	struct led_classdev cdev;
	struct mutex lock;
};

static int lm3643_write(struct lm3643 *priv, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(priv->client, reg, val);

	if (ret)
		dev_err(&priv->client->dev, "write 0x%02x=0x%02x failed: %d\n",
			reg, val, ret);
	return ret;
}

static void lm3643_check_flags(struct lm3643 *priv)
{
	/* reading the flag registers clears them */
	int f1 = i2c_smbus_read_byte_data(priv->client, LM3643_REG_FLAGS1);
	int f2 = i2c_smbus_read_byte_data(priv->client, LM3643_REG_FLAGS2);

	if (f1 < 0 || f2 < 0)
		return;
	f1 &= ~LM3643_FLAGS1_TX;
	if (f1 || f2)
		dev_warn_ratelimited(&priv->client->dev,
				     "fault flags1=0x%02x flags2=0x%02x\n", f1, f2);
}

static int lm3643_brightness_set(struct led_classdev *cdev,
				 enum led_brightness brightness)
{
	struct lm3643 *priv = container_of(cdev, struct lm3643, cdev);
	int ret;

	mutex_lock(&priv->lock);
	if (brightness == LED_OFF) {
		ret = lm3643_write(priv, LM3643_REG_ENABLE, 0x00);
	} else {
		ret = lm3643_write(priv, LM3643_REG_LED1_TORCH,
				   LM3643_TORCH_OVERRIDE | (brightness & 0x7f));
		if (!ret)
			ret = lm3643_write(priv, LM3643_REG_ENABLE,
					   LM3643_MODE_TORCH | LM3643_ENABLE_LED1 |
					   LM3643_ENABLE_LED2);
		lm3643_check_flags(priv);
	}
	mutex_unlock(&priv->lock);
	return ret;
}

static int lm3643_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lm3643 *priv;
	int id, ret;

	id = i2c_smbus_read_byte_data(client, LM3643_REG_DEVICE_ID);
	if (id < 0)
		return dev_err_probe(dev, id, "cannot read device id\n");
	if (id & LM3643_ID_MASK)
		return dev_err_probe(dev, -ENODEV, "unexpected device id 0x%02x\n", id);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->client = client;
	mutex_init(&priv->lock);
	i2c_set_clientdata(client, priv);

	/* standby, TX pin disabled */
	ret = lm3643_write(priv, LM3643_REG_ENABLE, 0x00);
	if (ret)
		return ret;
	lm3643_check_flags(priv);

	priv->cdev.name = "lm3643:ir:flood";
	priv->cdev.max_brightness = clamp_val(max_code, 1, 127);
	priv->cdev.brightness_set_blocking = lm3643_brightness_set;
	priv->cdev.flags = LED_CORE_SUSPENDRESUME;

	ret = devm_led_classdev_register(dev, &priv->cdev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register LED\n");

	dev_info(dev, "LM3643 rev %d, IR flood LED max code %u\n",
		 id & 0x7, priv->cdev.max_brightness);
	return 0;
}

static void lm3643_shutdown(struct i2c_client *client)
{
	i2c_smbus_write_byte_data(client, LM3643_REG_ENABLE, 0x00);
}

static const struct acpi_device_id lm3643_acpi_ids[] = {
	{ "TXNW3643" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, lm3643_acpi_ids);

static struct i2c_driver lm3643_driver = {
	.driver = {
		.name = "lm3643-ir",
		.acpi_match_table = lm3643_acpi_ids,
	},
	.probe = lm3643_probe,
	.shutdown = lm3643_shutdown,
};
module_i2c_driver(lm3643_driver);

MODULE_DESCRIPTION("TI LM3643 IR flood LED (torch mode)");
MODULE_LICENSE("GPL");
