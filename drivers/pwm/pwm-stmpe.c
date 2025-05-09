// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016 Linaro Ltd.
 *
 * Author: Linus Walleij <linus.walleij@linaro.org>
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/mfd/stmpe.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>

#define STMPE24XX_PWMCS		0x30
#define PWMCS_EN_PWM0		BIT(0)
#define PWMCS_EN_PWM1		BIT(1)
#define PWMCS_EN_PWM2		BIT(2)
#define STMPE24XX_PWMIC0	0x38
#define STMPE24XX_PWMIC1	0x39
#define STMPE24XX_PWMIC2	0x3a

#define STMPE_PWM_24XX_PINBASE	21

struct stmpe_pwm {
	struct stmpe *stmpe;
	u8 last_duty;
};

static inline struct stmpe_pwm *to_stmpe_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int stmpe_24xx_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct stmpe_pwm *stmpe_pwm = to_stmpe_pwm(chip);
	u8 value;
	int ret;

	ret = stmpe_reg_read(stmpe_pwm->stmpe, STMPE24XX_PWMCS);
	if (ret < 0) {
		dev_dbg(pwmchip_parent(chip), "error reading PWM#%u control\n",
			pwm->hwpwm);
		return ret;
	}

	value = ret | BIT(pwm->hwpwm);

	ret = stmpe_reg_write(stmpe_pwm->stmpe, STMPE24XX_PWMCS, value);
	if (ret) {
		dev_dbg(pwmchip_parent(chip), "error writing PWM#%u control\n",
			pwm->hwpwm);
		return ret;
	}

	return 0;
}

static int stmpe_24xx_pwm_disable(struct pwm_chip *chip,
				  struct pwm_device *pwm)
{
	struct stmpe_pwm *stmpe_pwm = to_stmpe_pwm(chip);
	u8 value;
	int ret;

	ret = stmpe_reg_read(stmpe_pwm->stmpe, STMPE24XX_PWMCS);
	if (ret < 0) {
		dev_dbg(pwmchip_parent(chip), "error reading PWM#%u control\n",
			pwm->hwpwm);
		return ret;
	}

	value = ret & ~BIT(pwm->hwpwm);

	ret = stmpe_reg_write(stmpe_pwm->stmpe, STMPE24XX_PWMCS, value);
	if (ret)
		dev_dbg(pwmchip_parent(chip), "error writing PWM#%u control\n",
			pwm->hwpwm);
	return ret;
}

/* STMPE 24xx PWM instructions */
#define SMAX		0x007f
#define SMIN		0x00ff
#define GTS		0x0000
#define LOAD		BIT(14) /* Only available on 2403 */
#define RAMPUP		0x0000
#define RAMPDOWN	BIT(7)
#define PRESCALE_512	BIT(14)
#define STEPTIME_1	BIT(8)
#define BRANCH		(BIT(15) | BIT(13))

static int stmpe_24xx_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
				 int duty_ns, int period_ns)
{
	struct stmpe_pwm *stmpe_pwm = to_stmpe_pwm(chip);
	unsigned int i, pin;
	u16 program[3] = {
		SMAX,
		GTS,
		GTS,
	};
	u8 offset;
	int ret;

	/* Make sure we are disabled */
	if (pwm_is_enabled(pwm)) {
		ret = stmpe_24xx_pwm_disable(chip, pwm);
		if (ret)
			return ret;
	} else {
		/* Connect the PWM to the pin */
		pin = pwm->hwpwm;

		/* On STMPE2401 and 2403 pins 21,22,23 are used */
		if (stmpe_pwm->stmpe->partnum == STMPE2401 ||
		    stmpe_pwm->stmpe->partnum == STMPE2403)
			pin += STMPE_PWM_24XX_PINBASE;

		ret = stmpe_set_altfunc(stmpe_pwm->stmpe, BIT(pin),
					STMPE_BLOCK_PWM);
		if (ret) {
			dev_err(pwmchip_parent(chip), "unable to connect PWM#%u to pin\n",
				pwm->hwpwm);
			return ret;
		}
	}

	/* STMPE24XX */
	switch (pwm->hwpwm) {
	case 0:
		offset = STMPE24XX_PWMIC0;
		break;

	case 1:
		offset = STMPE24XX_PWMIC1;
		break;

	case 2:
		offset = STMPE24XX_PWMIC2;
		break;

	default:
		/* Should not happen as npwm is 3 */
		return -ENODEV;
	}

	dev_dbg(pwmchip_parent(chip), "PWM#%u: config duty %d ns, period %d ns\n",
		pwm->hwpwm, duty_ns, period_ns);

	if (duty_ns == 0) {
		if (stmpe_pwm->stmpe->partnum == STMPE2401)
			program[0] = SMAX; /* off all the time */

		if (stmpe_pwm->stmpe->partnum == STMPE2403)
			program[0] = LOAD | 0xff; /* LOAD 0xff */

		stmpe_pwm->last_duty = 0x00;
	} else if (duty_ns == period_ns) {
		if (stmpe_pwm->stmpe->partnum == STMPE2401)
			program[0] = SMIN; /* on all the time */

		if (stmpe_pwm->stmpe->partnum == STMPE2403)
			program[0] = LOAD | 0x00; /* LOAD 0x00 */

		stmpe_pwm->last_duty = 0xff;
	} else {
		u8 value, last = stmpe_pwm->last_duty;
		unsigned long duty;

		/*
		 * Counter goes from 0x00 to 0xff repeatedly at 32768 Hz,
		 * (means a period of 30517 ns) then this is compared to the
		 * counter from the ramp, if this is >= PWM counter the output
		 * is high. With LOAD we can define how much of the cycle it
		 * is on.
		 *
		 * Prescale = 0 -> 2 kHz -> T = 1/f = 488281.25 ns
		 */

		/* Scale to 0..0xff */
		duty = duty_ns * 256;
		duty = DIV_ROUND_CLOSEST(duty, period_ns);
		value = duty;

		if (value == last) {
			/* Run the old program */
			if (pwm_is_enabled(pwm))
				stmpe_24xx_pwm_enable(chip, pwm);

			return 0;
		} else if (stmpe_pwm->stmpe->partnum == STMPE2403) {
			/* STMPE2403 can simply set the right PWM value */
			program[0] = LOAD | value;
			program[1] = 0x0000;
		} else if (stmpe_pwm->stmpe->partnum == STMPE2401) {
			/* STMPE2401 need a complex program */
			u16 incdec = 0x0000;

			if (last < value)
				/* Count up */
				incdec = RAMPUP | (value - last);
			else
				/* Count down */
				incdec = RAMPDOWN | (last - value);

			/* Step to desired value, smoothly */
			program[0] = PRESCALE_512 | STEPTIME_1 | incdec;

			/* Loop eternally to 0x00 */
			program[1] = BRANCH;
		}

		dev_dbg(pwmchip_parent(chip),
			"PWM#%u: value = %02x, last_duty = %02x, program=%04x,%04x,%04x\n",
			pwm->hwpwm, value, last, program[0], program[1],
			program[2]);
		stmpe_pwm->last_duty = value;
	}

	/*
	 * We can write programs of up to 64 16-bit words into this channel.
	 */
	for (i = 0; i < ARRAY_SIZE(program); i++) {
		u8 value;

		value = (program[i] >> 8) & 0xff;

		ret = stmpe_reg_write(stmpe_pwm->stmpe, offset, value);
		if (ret) {
			dev_dbg(pwmchip_parent(chip), "error writing register %02x: %d\n",
				offset, ret);
			return ret;
		}

		value = program[i] & 0xff;

		ret = stmpe_reg_write(stmpe_pwm->stmpe, offset, value);
		if (ret) {
			dev_dbg(pwmchip_parent(chip), "error writing register %02x: %d\n",
				offset, ret);
			return ret;
		}
	}

	/* If we were enabled, re-enable this PWM */
	if (pwm_is_enabled(pwm))
		stmpe_24xx_pwm_enable(chip, pwm);

	/* Sleep for 200ms so we're sure it will take effect */
	msleep(200);

	dev_dbg(pwmchip_parent(chip), "programmed PWM#%u, %u bytes\n", pwm->hwpwm, i);

	return 0;
}

static int stmpe_24xx_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
				const struct pwm_state *state)
{
	int err;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled) {
		if (pwm->state.enabled)
			return stmpe_24xx_pwm_disable(chip, pwm);

		return 0;
	}

	err = stmpe_24xx_pwm_config(chip, pwm, state->duty_cycle, state->period);
	if (err)
		return err;

	if (!pwm->state.enabled)
		err = stmpe_24xx_pwm_enable(chip, pwm);

	return err;
}

static const struct pwm_ops stmpe_24xx_pwm_ops = {
	.apply = stmpe_24xx_pwm_apply,
};

static int __init stmpe_pwm_probe(struct platform_device *pdev)
{
	struct stmpe *stmpe = dev_get_drvdata(pdev->dev.parent);
	struct pwm_chip *chip;
	struct stmpe_pwm *stmpe_pwm;
	int ret;

	switch (stmpe->partnum) {
	case STMPE2401:
	case STMPE2403:
		break;
	case STMPE1601:
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "STMPE1601 not yet supported\n");
	default:
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "Unknown STMPE PWM\n");
	}

	chip = devm_pwmchip_alloc(&pdev->dev, 3, sizeof(*stmpe_pwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	stmpe_pwm = to_stmpe_pwm(chip);

	stmpe_pwm->stmpe = stmpe;

	chip->ops = &stmpe_24xx_pwm_ops;

	ret = stmpe_enable(stmpe, STMPE_BLOCK_PWM);
	if (ret)
		return ret;

	ret = pwmchip_add(chip);
	if (ret) {
		stmpe_disable(stmpe, STMPE_BLOCK_PWM);
		return ret;
	}

	platform_set_drvdata(pdev, chip);

	return 0;
}

static void __exit stmpe_pwm_remove(struct platform_device *pdev)
{
	struct stmpe *stmpe = dev_get_drvdata(pdev->dev.parent);
	struct pwm_chip *chip = platform_get_drvdata(pdev);

	pwmchip_remove(chip);
	stmpe_disable(stmpe, STMPE_BLOCK_PWM);
}

/*
 * stmpe_pwm_remove() lives in .exit.text. For drivers registered via
 * module_platform_driver_probe() this is ok because they cannot get unbound at
 * runtime. So mark the driver struct with __refdata to prevent modpost
 * triggering a section mismatch warning.
 */
static struct platform_driver stmpe_pwm_driver __refdata = {
	.driver = {
		.name = "stmpe-pwm",
	},
	.remove = __exit_p(stmpe_pwm_remove),
};
module_platform_driver_probe(stmpe_pwm_driver, stmpe_pwm_probe);

MODULE_DESCRIPTION("STMPE expander PWM");
MODULE_LICENSE("GPL");
