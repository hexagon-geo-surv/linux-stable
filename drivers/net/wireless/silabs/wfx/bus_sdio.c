// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDIO interface.
 *
 * Copyright (c) 2017-2020, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 */
#include <linux/module.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/irq.h>
#include <linux/align.h>
#include <linux/pm.h>

#include "bus.h"
#include "wfx.h"
#include "hwio.h"
#include "main.h"
#include "bh.h"

static const struct wfx_platform_data pdata_wf200 = {
	.file_fw = "wfx/wfm_wf200",
	.file_pds = "wfx/wf200.pds",
};

static const struct wfx_platform_data pdata_brd4001a = {
	.file_fw = "wfx/wfm_wf200",
	.file_pds = "wfx/brd4001a.pds",
};

static const struct wfx_platform_data pdata_brd8022a = {
	.file_fw = "wfx/wfm_wf200",
	.file_pds = "wfx/brd8022a.pds",
};

static const struct wfx_platform_data pdata_brd8023a = {
	.file_fw = "wfx/wfm_wf200",
	.file_pds = "wfx/brd8023a.pds",
};

struct wfx_sdio_priv {
	struct sdio_func *func;
	struct wfx_dev *core;
	u8 buf_id_tx;
	u8 buf_id_rx;
	int of_irq;
};

static int wfx_sdio_copy_from_io(void *priv, unsigned int reg_id, void *dst, size_t count)
{
	struct wfx_sdio_priv *bus = priv;
	unsigned int sdio_addr = reg_id << 2;
	int ret;

	WARN(reg_id > 7, "chip only has 7 registers");
	WARN(!IS_ALIGNED((uintptr_t)dst, 4), "unaligned buffer address");
	WARN(!IS_ALIGNED(count, 4), "unaligned buffer size");

	/* Use queue mode buffers */
	if (reg_id == WFX_REG_IN_OUT_QUEUE)
		sdio_addr |= (bus->buf_id_rx + 1) << 7;
	ret = sdio_memcpy_fromio(bus->func, dst, sdio_addr, count);
	if (!ret && reg_id == WFX_REG_IN_OUT_QUEUE)
		bus->buf_id_rx = (bus->buf_id_rx + 1) % 4;

	return ret;
}

static int wfx_sdio_copy_to_io(void *priv, unsigned int reg_id, const void *src, size_t count)
{
	struct wfx_sdio_priv *bus = priv;
	unsigned int sdio_addr = reg_id << 2;
	int ret;

	WARN(reg_id > 7, "chip only has 7 registers");
	WARN(!IS_ALIGNED((uintptr_t)src, 4), "unaligned buffer address");
	WARN(!IS_ALIGNED(count, 4), "unaligned buffer size");

	/* Use queue mode buffers */
	if (reg_id == WFX_REG_IN_OUT_QUEUE)
		sdio_addr |= bus->buf_id_tx << 7;
	/* FIXME: discards 'const' qualifier for src */
	ret = sdio_memcpy_toio(bus->func, sdio_addr, (void *)src, count);
	if (!ret && reg_id == WFX_REG_IN_OUT_QUEUE)
		bus->buf_id_tx = (bus->buf_id_tx + 1) % 32;

	return ret;
}

static void wfx_sdio_lock(void *priv)
{
	struct wfx_sdio_priv *bus = priv;

	sdio_claim_host(bus->func);
}

static void wfx_sdio_unlock(void *priv)
{
	struct wfx_sdio_priv *bus = priv;

	sdio_release_host(bus->func);
}

static void wfx_sdio_irq_handler(struct sdio_func *func)
{
	struct wfx_sdio_priv *bus = sdio_get_drvdata(func);

	wfx_bh_request_rx(bus->core);
}

static irqreturn_t wfx_sdio_irq_handler_ext(int irq, void *priv)
{
	struct wfx_sdio_priv *bus = priv;

	sdio_claim_host(bus->func);
	wfx_bh_request_rx(bus->core);
	sdio_release_host(bus->func);
	return IRQ_HANDLED;
}

static int wfx_sdio_irq_subscribe(void *priv)
{
	struct wfx_sdio_priv *bus = priv;
	u32 flags;
	int ret;
	u8 cccr;

	if (!bus->of_irq) {
		sdio_claim_host(bus->func);
		ret = sdio_claim_irq(bus->func, wfx_sdio_irq_handler);
		sdio_release_host(bus->func);
		return ret;
	}

	flags = irq_get_trigger_type(bus->of_irq);
	if (!flags)
		flags = IRQF_TRIGGER_HIGH;
	flags |= IRQF_ONESHOT;
	ret = devm_request_threaded_irq(&bus->func->dev, bus->of_irq, NULL,
					wfx_sdio_irq_handler_ext, flags, "wfx", bus);
	if (ret)
		return ret;
	sdio_claim_host(bus->func);
	cccr = sdio_f0_readb(bus->func, SDIO_CCCR_IENx, NULL);
	cccr |= BIT(0);
	cccr |= BIT(bus->func->num);
	sdio_f0_writeb(bus->func, cccr, SDIO_CCCR_IENx, NULL);
	sdio_release_host(bus->func);
	return 0;
}

static int wfx_sdio_irq_unsubscribe(void *priv)
{
	struct wfx_sdio_priv *bus = priv;
	int ret;

	if (bus->of_irq)
		devm_free_irq(&bus->func->dev, bus->of_irq, bus);
	sdio_claim_host(bus->func);
	ret = sdio_release_irq(bus->func);
	sdio_release_host(bus->func);
	return ret;
}

static size_t wfx_sdio_align_size(void *priv, size_t size)
{
	struct wfx_sdio_priv *bus = priv;

	return sdio_align_size(bus->func, size);
}

static void wfx_sdio_set_wakeup(void *priv, bool enabled)
{
	struct wfx_sdio_priv *bus = priv;

	device_set_wakeup_enable(&bus->func->dev, enabled);
}

static const struct wfx_hwbus_ops wfx_sdio_hwbus_ops = {
	.copy_from_io    = wfx_sdio_copy_from_io,
	.copy_to_io      = wfx_sdio_copy_to_io,
	.irq_subscribe   = wfx_sdio_irq_subscribe,
	.irq_unsubscribe = wfx_sdio_irq_unsubscribe,
	.lock            = wfx_sdio_lock,
	.unlock          = wfx_sdio_unlock,
	.align_size      = wfx_sdio_align_size,
	.set_wakeup      = wfx_sdio_set_wakeup,
};

static const struct of_device_id wfx_sdio_of_match[] = {
	{ .compatible = "silabs,wf200",    .data = &pdata_wf200 },
	{ .compatible = "silabs,brd4001a", .data = &pdata_brd4001a },
	{ .compatible = "silabs,brd8022a", .data = &pdata_brd8022a },
	{ .compatible = "silabs,brd8023a", .data = &pdata_brd8023a },
	{ },
};
MODULE_DEVICE_TABLE(of, wfx_sdio_of_match);

static int wfx_sdio_suspend(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct wfx_sdio_priv *bus = sdio_get_drvdata(func);
	int ret;

	if (!device_may_wakeup(dev))
		return 0;

	flush_work(&bus->core->hif.bh);
	/* Either "wakeup-source" attribute or out-of-band IRQ is required for
	 * WoWLAN
	 */
	if (bus->of_irq) {
		ret = enable_irq_wake(bus->of_irq);
		if (ret)
			return ret;
	} else {
		ret = sdio_set_host_pm_flags(func, MMC_PM_WAKE_SDIO_IRQ);
		if (ret)
			return ret;
	}
	return sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);
}

static int wfx_sdio_resume(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct wfx_sdio_priv *bus = sdio_get_drvdata(func);

	if (!device_may_wakeup(dev))
		return 0;
	if (bus->of_irq)
		return disable_irq_wake(bus->of_irq);
	else
		return 0;
}

static int wfx_sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	const struct wfx_platform_data *pdata = of_device_get_match_data(&func->dev);
	mmc_pm_flag_t pm_flag = sdio_get_host_pm_caps(func);
	struct device_node *np = func->dev.of_node;
	struct wfx_sdio_priv *bus;
	int ret;

	if (func->num != 1) {
		dev_err(&func->dev, "SDIO function number is %d while it should always be 1 (unsupported chip?)\n",
			func->num);
		return -ENODEV;
	}

	if (!pdata) {
		dev_warn(&func->dev, "no compatible device found in DT\n");
		return -ENODEV;
	}

	bus = devm_kzalloc(&func->dev, sizeof(*bus), GFP_KERNEL);
	if (!bus)
		return -ENOMEM;

	bus->func = func;
	bus->of_irq = irq_of_parse_and_map(np, 0);
	sdio_set_drvdata(func, bus);

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	/* Block of 64 bytes is more efficient than 512B for frame sizes < 4k */
	sdio_set_block_size(func, 64);
	sdio_release_host(func);
	if (ret)
		return ret;

	bus->core = wfx_init_common(&func->dev, pdata, &wfx_sdio_hwbus_ops, bus);
	if (!bus->core) {
		ret = -EIO;
		goto sdio_release;
	}

	ret = wfx_probe(bus->core);
	if (ret)
		goto sdio_release;

	if (pm_flag & MMC_PM_KEEP_POWER)
		device_set_wakeup_capable(&func->dev, true);

	return 0;

sdio_release:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	return ret;
}

static void wfx_sdio_remove(struct sdio_func *func)
{
	struct wfx_sdio_priv *bus = sdio_get_drvdata(func);

	wfx_release(bus->core);
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
}

static const struct sdio_device_id wfx_sdio_ids[] = {
	/* WF200 does not have official VID/PID */
	{ SDIO_DEVICE(0x0000, 0x1000) },
	{ },
};
MODULE_DEVICE_TABLE(sdio, wfx_sdio_ids);

static DEFINE_SIMPLE_DEV_PM_OPS(wfx_sdio_pm_ops, wfx_sdio_suspend, wfx_sdio_resume);

struct sdio_driver wfx_sdio_driver = {
	.name = "wfx-sdio",
	.id_table = wfx_sdio_ids,
	.probe = wfx_sdio_probe,
	.remove = wfx_sdio_remove,
	.drv = {
		.of_match_table = wfx_sdio_of_match,
		.pm = &wfx_sdio_pm_ops,
	}
};
