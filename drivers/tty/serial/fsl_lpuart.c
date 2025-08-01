// SPDX-License-Identifier: GPL-2.0+
/*
 *  Freescale lpuart serial port driver
 *
 *  Copyright 2012-2014 Freescale Semiconductor, Inc.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/circ_buf.h>
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/tty_flip.h>

/* All registers are 8-bit width */
#define UARTBDH			0x00
#define UARTBDL			0x01
#define UARTCR1			0x02
#define UARTCR2			0x03
#define UARTSR1			0x04
#define UARTCR3			0x06
#define UARTDR			0x07
#define UARTCR4			0x0a
#define UARTCR5			0x0b
#define UARTMODEM		0x0d
#define UARTPFIFO		0x10
#define UARTCFIFO		0x11
#define UARTSFIFO		0x12
#define UARTTWFIFO		0x13
#define UARTTCFIFO		0x14
#define UARTRWFIFO		0x15

#define UARTBDH_LBKDIE		0x80
#define UARTBDH_RXEDGIE		0x40
#define UARTBDH_SBR_MASK	0x1f

#define UARTCR1_LOOPS		0x80
#define UARTCR1_RSRC		0x20
#define UARTCR1_M		0x10
#define UARTCR1_WAKE		0x08
#define UARTCR1_ILT		0x04
#define UARTCR1_PE		0x02
#define UARTCR1_PT		0x01

#define UARTCR2_TIE		0x80
#define UARTCR2_TCIE		0x40
#define UARTCR2_RIE		0x20
#define UARTCR2_ILIE		0x10
#define UARTCR2_TE		0x08
#define UARTCR2_RE		0x04
#define UARTCR2_RWU		0x02
#define UARTCR2_SBK		0x01

#define UARTSR1_TDRE		0x80
#define UARTSR1_TC		0x40
#define UARTSR1_RDRF		0x20
#define UARTSR1_IDLE		0x10
#define UARTSR1_OR		0x08
#define UARTSR1_NF		0x04
#define UARTSR1_FE		0x02
#define UARTSR1_PE		0x01

#define UARTCR3_R8		0x80
#define UARTCR3_T8		0x40
#define UARTCR3_TXDIR		0x20
#define UARTCR3_TXINV		0x10
#define UARTCR3_ORIE		0x08
#define UARTCR3_NEIE		0x04
#define UARTCR3_FEIE		0x02
#define UARTCR3_PEIE		0x01

#define UARTCR4_MAEN1		0x80
#define UARTCR4_MAEN2		0x40
#define UARTCR4_M10		0x20
#define UARTCR4_BRFA_MASK	0x1f
#define UARTCR4_BRFA_OFF	0

#define UARTCR5_TDMAS		0x80
#define UARTCR5_RDMAS		0x20

#define UARTMODEM_RXRTSE	0x08
#define UARTMODEM_TXRTSPOL	0x04
#define UARTMODEM_TXRTSE	0x02
#define UARTMODEM_TXCTSE	0x01

#define UARTPFIFO_TXFE		0x80
#define UARTPFIFO_FIFOSIZE_MASK	0x7
#define UARTPFIFO_TXSIZE_OFF	4
#define UARTPFIFO_RXFE		0x08
#define UARTPFIFO_RXSIZE_OFF	0

#define UARTCFIFO_TXFLUSH	0x80
#define UARTCFIFO_RXFLUSH	0x40
#define UARTCFIFO_RXOFE		0x04
#define UARTCFIFO_TXOFE		0x02
#define UARTCFIFO_RXUFE		0x01

#define UARTSFIFO_TXEMPT	0x80
#define UARTSFIFO_RXEMPT	0x40
#define UARTSFIFO_RXOF		0x04
#define UARTSFIFO_TXOF		0x02
#define UARTSFIFO_RXUF		0x01

/* 32-bit global registers only for i.MX7ULP/i.MX8x
 * Used to reset all internal logic and registers, except the Global Register.
 */
#define UART_GLOBAL		0x8

/* 32-bit register definition */
#define UARTBAUD		0x00
#define UARTSTAT		0x04
#define UARTCTRL		0x08
#define UARTDATA		0x0C
#define UARTMATCH		0x10
#define UARTMODIR		0x14
#define UARTFIFO		0x18
#define UARTWATER		0x1c

#define UARTBAUD_MAEN1		0x80000000
#define UARTBAUD_MAEN2		0x40000000
#define UARTBAUD_M10		0x20000000
#define UARTBAUD_TDMAE		0x00800000
#define UARTBAUD_RDMAE		0x00200000
#define UARTBAUD_MATCFG		0x00400000
#define UARTBAUD_BOTHEDGE	0x00020000
#define UARTBAUD_RESYNCDIS	0x00010000
#define UARTBAUD_LBKDIE		0x00008000
#define UARTBAUD_RXEDGIE	0x00004000
#define UARTBAUD_SBNS		0x00002000
#define UARTBAUD_SBR		0x00000000
#define UARTBAUD_SBR_MASK	0x1fff
#define UARTBAUD_OSR_MASK       0x1f
#define UARTBAUD_OSR_SHIFT      24

#define UARTSTAT_LBKDIF		0x80000000
#define UARTSTAT_RXEDGIF	0x40000000
#define UARTSTAT_MSBF		0x20000000
#define UARTSTAT_RXINV		0x10000000
#define UARTSTAT_RWUID		0x08000000
#define UARTSTAT_BRK13		0x04000000
#define UARTSTAT_LBKDE		0x02000000
#define UARTSTAT_RAF		0x01000000
#define UARTSTAT_TDRE		0x00800000
#define UARTSTAT_TC		0x00400000
#define UARTSTAT_RDRF		0x00200000
#define UARTSTAT_IDLE		0x00100000
#define UARTSTAT_OR		0x00080000
#define UARTSTAT_NF		0x00040000
#define UARTSTAT_FE		0x00020000
#define UARTSTAT_PE		0x00010000
#define UARTSTAT_MA1F		0x00008000
#define UARTSTAT_M21F		0x00004000

#define UARTCTRL_R8T9		0x80000000
#define UARTCTRL_R9T8		0x40000000
#define UARTCTRL_TXDIR		0x20000000
#define UARTCTRL_TXINV		0x10000000
#define UARTCTRL_ORIE		0x08000000
#define UARTCTRL_NEIE		0x04000000
#define UARTCTRL_FEIE		0x02000000
#define UARTCTRL_PEIE		0x01000000
#define UARTCTRL_TIE		0x00800000
#define UARTCTRL_TCIE		0x00400000
#define UARTCTRL_RIE		0x00200000
#define UARTCTRL_ILIE		0x00100000
#define UARTCTRL_TE		0x00080000
#define UARTCTRL_RE		0x00040000
#define UARTCTRL_RWU		0x00020000
#define UARTCTRL_SBK		0x00010000
#define UARTCTRL_MA1IE		0x00008000
#define UARTCTRL_MA2IE		0x00004000
#define UARTCTRL_M7		0x00000800
#define UARTCTRL_IDLECFG	GENMASK(10, 8)
#define UARTCTRL_LOOPS		0x00000080
#define UARTCTRL_DOZEEN		0x00000040
#define UARTCTRL_RSRC		0x00000020
#define UARTCTRL_M		0x00000010
#define UARTCTRL_WAKE		0x00000008
#define UARTCTRL_ILT		0x00000004
#define UARTCTRL_PE		0x00000002
#define UARTCTRL_PT		0x00000001

#define UARTDATA_NOISY		0x00008000
#define UARTDATA_PARITYE	0x00004000
#define UARTDATA_FRETSC		0x00002000
#define UARTDATA_RXEMPT		0x00001000
#define UARTDATA_IDLINE		0x00000800
#define UARTDATA_MASK		0x3ff

#define UARTMODIR_IREN		0x00020000
#define UARTMODIR_RTSWATER	GENMASK(10, 8)
#define UARTMODIR_TXCTSSRC	0x00000020
#define UARTMODIR_TXCTSC	0x00000010
#define UARTMODIR_RXRTSE	0x00000008
#define UARTMODIR_TXRTSPOL	0x00000004
#define UARTMODIR_TXRTSE	0x00000002
#define UARTMODIR_TXCTSE	0x00000001

#define UARTFIFO_TXEMPT		0x00800000
#define UARTFIFO_RXEMPT		0x00400000
#define UARTFIFO_TXOF		0x00020000
#define UARTFIFO_RXUF		0x00010000
#define UARTFIFO_TXFLUSH	0x00008000
#define UARTFIFO_RXFLUSH	0x00004000
#define UARTFIFO_RXIDEN	GENMASK(12, 10)
#define UARTFIFO_TXOFE		0x00000200
#define UARTFIFO_RXUFE		0x00000100
#define UARTFIFO_TXFE		0x00000080
#define UARTFIFO_FIFOSIZE_MASK	0x7
#define UARTFIFO_TXSIZE_OFF	4
#define UARTFIFO_RXFE		0x00000008
#define UARTFIFO_RXSIZE_OFF	0
#define UARTFIFO_DEPTH(x)	(0x1 << ((x) ? ((x) + 1) : 0))

#define UARTWATER_COUNT_MASK	0xff
#define UARTWATER_TXCNT_OFF	8
#define UARTWATER_RXCNT_OFF	24
#define UARTWATER_WATER_MASK	0xff
#define UARTWATER_TXWATER_OFF	0
#define UARTWATER_RXWATER_OFF	16

#define UART_GLOBAL_RST	0x2
#define GLOBAL_RST_MIN_US	20
#define GLOBAL_RST_MAX_US	40

/* Rx DMA timeout in ms, which is used to calculate Rx ring buffer size */
#define DMA_RX_TIMEOUT		(10)
#define DMA_RX_IDLE_CHARS	8
#define UART_AUTOSUSPEND_TIMEOUT	3000

#define DRIVER_NAME	"fsl-lpuart"
#define DEV_NAME	"ttyLP"
#define UART_NR		12

/* IMX lpuart has four extra unused regs located at the beginning */
#define IMX_REG_OFF	0x10

enum lpuart_type {
	VF610_LPUART,
	LS1021A_LPUART,
	LS1028A_LPUART,
	IMX7ULP_LPUART,
	IMX8ULP_LPUART,
	IMX8QXP_LPUART,
	IMXRT1050_LPUART,
};

struct lpuart_port {
	struct uart_port	port;
	enum lpuart_type	devtype;
	struct clk		*ipg_clk;
	struct clk		*baud_clk;
	unsigned int		txfifo_size;
	unsigned int		rxfifo_size;

	u8			rx_watermark;
	bool			lpuart_dma_tx_use;
	bool			lpuart_dma_rx_use;
	struct dma_chan		*dma_tx_chan;
	struct dma_chan		*dma_rx_chan;
	struct dma_async_tx_descriptor  *dma_tx_desc;
	struct dma_async_tx_descriptor  *dma_rx_desc;
	dma_cookie_t		dma_tx_cookie;
	dma_cookie_t		dma_rx_cookie;
	unsigned int		dma_tx_bytes;
	unsigned int		dma_rx_bytes;
	bool			dma_tx_in_progress;
	unsigned int		dma_rx_timeout;
	struct timer_list	lpuart_timer;
	struct scatterlist	rx_sgl, tx_sgl[2];
	struct circ_buf		rx_ring;
	int			rx_dma_rng_buf_len;
	int                     last_residue;
	unsigned int		dma_tx_nents;
	wait_queue_head_t	dma_wait;
	bool			is_cs7; /* Set to true when character size is 7 */
					/* and the parity is enabled		*/
	bool			dma_idle_int;
};

struct lpuart_soc_data {
	enum lpuart_type devtype;
	char iotype;
	u8 reg_off;
	u8 rx_watermark;
};

static const struct lpuart_soc_data vf_data = {
	.devtype = VF610_LPUART,
	.iotype = UPIO_MEM,
	.rx_watermark = 1,
};

static const struct lpuart_soc_data ls1021a_data = {
	.devtype = LS1021A_LPUART,
	.iotype = UPIO_MEM32BE,
	.rx_watermark = 1,
};

static const struct lpuart_soc_data ls1028a_data = {
	.devtype = LS1028A_LPUART,
	.iotype = UPIO_MEM32,
	.rx_watermark = 0,
};

static const struct lpuart_soc_data imx7ulp_data = {
	.devtype = IMX7ULP_LPUART,
	.iotype = UPIO_MEM32,
	.reg_off = IMX_REG_OFF,
	.rx_watermark = 1,
};

static const struct lpuart_soc_data imx8ulp_data = {
	.devtype = IMX8ULP_LPUART,
	.iotype = UPIO_MEM32,
	.reg_off = IMX_REG_OFF,
	.rx_watermark = 3,
};

static const struct lpuart_soc_data imx8qxp_data = {
	.devtype = IMX8QXP_LPUART,
	.iotype = UPIO_MEM32,
	.reg_off = IMX_REG_OFF,
	.rx_watermark = 7, /* A lower watermark is ideal for low baud rates. */
};
static const struct lpuart_soc_data imxrt1050_data = {
	.devtype = IMXRT1050_LPUART,
	.iotype = UPIO_MEM32,
	.reg_off = IMX_REG_OFF,
	.rx_watermark = 1,
};

static const struct of_device_id lpuart_dt_ids[] = {
	{ .compatible = "fsl,vf610-lpuart",	.data = &vf_data, },
	{ .compatible = "fsl,ls1021a-lpuart",	.data = &ls1021a_data, },
	{ .compatible = "fsl,ls1028a-lpuart",	.data = &ls1028a_data, },
	{ .compatible = "fsl,imx7ulp-lpuart",	.data = &imx7ulp_data, },
	{ .compatible = "fsl,imx8ulp-lpuart",	.data = &imx8ulp_data, },
	{ .compatible = "fsl,imx8qxp-lpuart",	.data = &imx8qxp_data, },
	{ .compatible = "fsl,imxrt1050-lpuart",	.data = &imxrt1050_data},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lpuart_dt_ids);

/* Forward declare this for the dma callbacks*/
static void lpuart_dma_tx_complete(void *arg);

static inline bool is_layerscape_lpuart(struct lpuart_port *sport)
{
	return (sport->devtype == LS1021A_LPUART ||
		sport->devtype == LS1028A_LPUART);
}

static inline bool is_imx7ulp_lpuart(struct lpuart_port *sport)
{
	return sport->devtype == IMX7ULP_LPUART;
}

static inline bool is_imx8ulp_lpuart(struct lpuart_port *sport)
{
	return sport->devtype == IMX8ULP_LPUART;
}

static inline bool is_imx8qxp_lpuart(struct lpuart_port *sport)
{
	return sport->devtype == IMX8QXP_LPUART;
}

static inline u32 lpuart32_read(struct uart_port *port, u32 off)
{
	switch (port->iotype) {
	case UPIO_MEM32:
		return readl(port->membase + off);
	case UPIO_MEM32BE:
		return ioread32be(port->membase + off);
	default:
		return 0;
	}
}

static inline void lpuart32_write(struct uart_port *port, u32 val,
				  u32 off)
{
	switch (port->iotype) {
	case UPIO_MEM32:
		writel(val, port->membase + off);
		break;
	case UPIO_MEM32BE:
		iowrite32be(val, port->membase + off);
		break;
	default:
		break;
	}
}

static int __lpuart_enable_clks(struct lpuart_port *sport, bool is_en)
{
	int ret = 0;

	if (is_en) {
		ret = clk_prepare_enable(sport->ipg_clk);
		if (ret)
			return ret;

		ret = clk_prepare_enable(sport->baud_clk);
		if (ret) {
			clk_disable_unprepare(sport->ipg_clk);
			return ret;
		}
	} else {
		clk_disable_unprepare(sport->baud_clk);
		clk_disable_unprepare(sport->ipg_clk);
	}

	return 0;
}

static unsigned int lpuart_get_baud_clk_rate(struct lpuart_port *sport)
{
	if (is_imx8qxp_lpuart(sport))
		return clk_get_rate(sport->baud_clk);

	return clk_get_rate(sport->ipg_clk);
}

#define lpuart_enable_clks(x)	__lpuart_enable_clks(x, true)
#define lpuart_disable_clks(x)	__lpuart_enable_clks(x, false)

static void lpuart_stop_tx(struct uart_port *port)
{
	u8 cr2;

	cr2 = readb(port->membase + UARTCR2);
	cr2 &= ~(UARTCR2_TIE | UARTCR2_TCIE);
	writeb(cr2, port->membase + UARTCR2);
}

static void lpuart32_stop_tx(struct uart_port *port)
{
	u32 ctrl;

	ctrl = lpuart32_read(port, UARTCTRL);
	ctrl &= ~(UARTCTRL_TIE | UARTCTRL_TCIE);
	lpuart32_write(port, ctrl, UARTCTRL);
}

static void lpuart_stop_rx(struct uart_port *port)
{
	u8 cr2;

	cr2 = readb(port->membase + UARTCR2);
	writeb(cr2 & ~UARTCR2_RE, port->membase + UARTCR2);
}

static void lpuart32_stop_rx(struct uart_port *port)
{
	u32 ctrl;

	ctrl = lpuart32_read(port, UARTCTRL);
	lpuart32_write(port, ctrl & ~UARTCTRL_RE, UARTCTRL);
}

static void lpuart_dma_tx(struct lpuart_port *sport)
{
	struct tty_port *tport = &sport->port.state->port;
	struct scatterlist *sgl = sport->tx_sgl;
	struct device *dev = sport->port.dev;
	struct dma_chan *chan = sport->dma_tx_chan;
	int ret;

	if (sport->dma_tx_in_progress)
		return;

	sg_init_table(sgl, ARRAY_SIZE(sport->tx_sgl));
	sport->dma_tx_bytes = kfifo_len(&tport->xmit_fifo);
	sport->dma_tx_nents = kfifo_dma_out_prepare(&tport->xmit_fifo, sgl,
			ARRAY_SIZE(sport->tx_sgl), sport->dma_tx_bytes);

	ret = dma_map_sg(chan->device->dev, sgl, sport->dma_tx_nents,
			 DMA_TO_DEVICE);
	if (!ret) {
		dev_err(dev, "DMA mapping error for TX.\n");
		return;
	}

	sport->dma_tx_desc = dmaengine_prep_slave_sg(chan, sgl,
					ret, DMA_MEM_TO_DEV,
					DMA_PREP_INTERRUPT);
	if (!sport->dma_tx_desc) {
		dma_unmap_sg(chan->device->dev, sgl, sport->dma_tx_nents,
			      DMA_TO_DEVICE);
		dev_err(dev, "Cannot prepare TX slave DMA!\n");
		return;
	}

	sport->dma_tx_desc->callback = lpuart_dma_tx_complete;
	sport->dma_tx_desc->callback_param = sport;
	sport->dma_tx_in_progress = true;
	sport->dma_tx_cookie = dmaengine_submit(sport->dma_tx_desc);
	dma_async_issue_pending(chan);
}

static bool lpuart_stopped_or_empty(struct uart_port *port)
{
	return kfifo_is_empty(&port->state->port.xmit_fifo) ||
		uart_tx_stopped(port);
}

static void lpuart_dma_tx_complete(void *arg)
{
	struct lpuart_port *sport = arg;
	struct scatterlist *sgl = &sport->tx_sgl[0];
	struct tty_port *tport = &sport->port.state->port;
	struct dma_chan *chan = sport->dma_tx_chan;
	unsigned long flags;

	uart_port_lock_irqsave(&sport->port, &flags);
	if (!sport->dma_tx_in_progress) {
		uart_port_unlock_irqrestore(&sport->port, flags);
		return;
	}

	dma_unmap_sg(chan->device->dev, sgl, sport->dma_tx_nents,
		     DMA_TO_DEVICE);

	uart_xmit_advance(&sport->port, sport->dma_tx_bytes);
	sport->dma_tx_in_progress = false;
	uart_port_unlock_irqrestore(&sport->port, flags);

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(&sport->port);

	if (waitqueue_active(&sport->dma_wait)) {
		wake_up(&sport->dma_wait);
		return;
	}

	uart_port_lock_irqsave(&sport->port, &flags);

	if (!lpuart_stopped_or_empty(&sport->port))
		lpuart_dma_tx(sport);

	uart_port_unlock_irqrestore(&sport->port, flags);
}

static dma_addr_t lpuart_dma_datareg_addr(struct lpuart_port *sport)
{
	switch (sport->port.iotype) {
	case UPIO_MEM32:
		return sport->port.mapbase + UARTDATA;
	case UPIO_MEM32BE:
		return sport->port.mapbase + UARTDATA + sizeof(u32) - 1;
	default:
		return sport->port.mapbase + UARTDR;
	}
}

static int lpuart_dma_tx_request(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port,
					struct lpuart_port, port);
	struct dma_slave_config dma_tx_sconfig = {};
	int ret;

	dma_tx_sconfig.dst_addr = lpuart_dma_datareg_addr(sport);
	dma_tx_sconfig.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	dma_tx_sconfig.dst_maxburst = 1;
	dma_tx_sconfig.direction = DMA_MEM_TO_DEV;
	ret = dmaengine_slave_config(sport->dma_tx_chan, &dma_tx_sconfig);

	if (ret) {
		dev_err(port->dev,
				"DMA slave config failed, err = %d\n", ret);
		return ret;
	}

	return 0;
}

static bool lpuart_is_32(struct lpuart_port *sport)
{
	return sport->port.iotype == UPIO_MEM32 ||
	       sport->port.iotype ==  UPIO_MEM32BE;
}

static void lpuart_flush_buffer(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port, struct lpuart_port, port);
	struct dma_chan *chan = sport->dma_tx_chan;
	u32 fifo;

	if (sport->lpuart_dma_tx_use) {
		if (sport->dma_tx_in_progress) {
			dma_unmap_sg(chan->device->dev, &sport->tx_sgl[0],
				sport->dma_tx_nents, DMA_TO_DEVICE);
			sport->dma_tx_in_progress = false;
		}
		dmaengine_terminate_async(chan);
	}

	if (lpuart_is_32(sport)) {
		fifo = lpuart32_read(port, UARTFIFO);
		fifo |= UARTFIFO_TXFLUSH | UARTFIFO_RXFLUSH;
		lpuart32_write(port, fifo, UARTFIFO);
	} else {
		fifo = readb(port->membase + UARTCFIFO);
		fifo |= UARTCFIFO_TXFLUSH | UARTCFIFO_RXFLUSH;
		writeb(fifo, port->membase + UARTCFIFO);
	}
}

static void lpuart_wait_bit_set(struct uart_port *port, unsigned int offset,
				u8 bit)
{
	while (!(readb(port->membase + offset) & bit))
		cpu_relax();
}

static void lpuart32_wait_bit_set(struct uart_port *port, unsigned int offset,
				  u32 bit)
{
	while (!(lpuart32_read(port, offset) & bit))
		cpu_relax();
}

#if defined(CONFIG_CONSOLE_POLL)

static int lpuart_poll_init(struct uart_port *port)
{
	unsigned long flags;
	u8 fifo;

	port->fifosize = 0;

	uart_port_lock_irqsave(port, &flags);
	/* Disable Rx & Tx */
	writeb(0, port->membase + UARTCR2);

	fifo = readb(port->membase + UARTPFIFO);
	/* Enable Rx and Tx FIFO */
	writeb(fifo | UARTPFIFO_RXFE | UARTPFIFO_TXFE,
			port->membase + UARTPFIFO);

	/* flush Tx and Rx FIFO */
	writeb(UARTCFIFO_TXFLUSH | UARTCFIFO_RXFLUSH,
			port->membase + UARTCFIFO);

	/* explicitly clear RDRF */
	if (readb(port->membase + UARTSR1) & UARTSR1_RDRF) {
		readb(port->membase + UARTDR);
		writeb(UARTSFIFO_RXUF, port->membase + UARTSFIFO);
	}

	writeb(0, port->membase + UARTTWFIFO);
	writeb(1, port->membase + UARTRWFIFO);

	/* Enable Rx and Tx */
	writeb(UARTCR2_RE | UARTCR2_TE, port->membase + UARTCR2);
	uart_port_unlock_irqrestore(port, flags);

	return 0;
}

static void lpuart_poll_put_char(struct uart_port *port, unsigned char c)
{
	/* drain */
	lpuart_wait_bit_set(port, UARTSR1, UARTSR1_TDRE);
	writeb(c, port->membase + UARTDR);
}

static int lpuart_poll_get_char(struct uart_port *port)
{
	if (!(readb(port->membase + UARTSR1) & UARTSR1_RDRF))
		return NO_POLL_CHAR;

	return readb(port->membase + UARTDR);
}

static int lpuart32_poll_init(struct uart_port *port)
{
	unsigned long flags;
	u32 fifo;

	port->fifosize = 0;

	uart_port_lock_irqsave(port, &flags);

	/* Disable Rx & Tx */
	lpuart32_write(port, 0, UARTCTRL);

	fifo = lpuart32_read(port, UARTFIFO);

	/* Enable Rx and Tx FIFO */
	lpuart32_write(port, fifo | UARTFIFO_RXFE | UARTFIFO_TXFE, UARTFIFO);

	/* flush Tx and Rx FIFO */
	lpuart32_write(port, UARTFIFO_TXFLUSH | UARTFIFO_RXFLUSH, UARTFIFO);

	/* explicitly clear RDRF */
	if (lpuart32_read(port, UARTSTAT) & UARTSTAT_RDRF) {
		lpuart32_read(port, UARTDATA);
		lpuart32_write(port, UARTFIFO_RXUF, UARTFIFO);
	}

	/* Enable Rx and Tx */
	lpuart32_write(port, UARTCTRL_RE | UARTCTRL_TE, UARTCTRL);
	uart_port_unlock_irqrestore(port, flags);

	return 0;
}

static void lpuart32_poll_put_char(struct uart_port *port, unsigned char c)
{
	lpuart32_wait_bit_set(port, UARTSTAT, UARTSTAT_TDRE);
	lpuart32_write(port, c, UARTDATA);
}

static int lpuart32_poll_get_char(struct uart_port *port)
{
	if (!(lpuart32_read(port, UARTWATER) >> UARTWATER_RXCNT_OFF))
		return NO_POLL_CHAR;

	return lpuart32_read(port, UARTDATA);
}
#endif

static inline void lpuart_transmit_buffer(struct lpuart_port *sport)
{
	struct uart_port *port = &sport->port;
	u8 ch;

	uart_port_tx(port, ch,
		readb(port->membase + UARTTCFIFO) < sport->txfifo_size,
		writeb(ch, port->membase + UARTDR));
}

static inline void lpuart32_transmit_buffer(struct lpuart_port *sport)
{
	struct tty_port *tport = &sport->port.state->port;
	u32 txcnt;
	unsigned char c;

	if (sport->port.x_char) {
		lpuart32_write(&sport->port, sport->port.x_char, UARTDATA);
		sport->port.icount.tx++;
		sport->port.x_char = 0;
		return;
	}

	if (lpuart_stopped_or_empty(&sport->port)) {
		lpuart32_stop_tx(&sport->port);
		return;
	}

	txcnt = lpuart32_read(&sport->port, UARTWATER);
	txcnt = txcnt >> UARTWATER_TXCNT_OFF;
	txcnt &= UARTWATER_COUNT_MASK;
	while (txcnt < sport->txfifo_size &&
			uart_fifo_get(&sport->port, &c)) {
		lpuart32_write(&sport->port, c, UARTDATA);
		txcnt = lpuart32_read(&sport->port, UARTWATER);
		txcnt = txcnt >> UARTWATER_TXCNT_OFF;
		txcnt &= UARTWATER_COUNT_MASK;
	}

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(&sport->port);

	if (kfifo_is_empty(&tport->xmit_fifo))
		lpuart32_stop_tx(&sport->port);
}

static void lpuart_start_tx(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port,
			struct lpuart_port, port);
	u8 cr2;

	cr2 = readb(port->membase + UARTCR2);
	writeb(cr2 | UARTCR2_TIE, port->membase + UARTCR2);

	if (sport->lpuart_dma_tx_use) {
		if (!lpuart_stopped_or_empty(port))
			lpuart_dma_tx(sport);
	} else {
		if (readb(port->membase + UARTSR1) & UARTSR1_TDRE)
			lpuart_transmit_buffer(sport);
	}
}

static void lpuart32_start_tx(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port, struct lpuart_port, port);
	u32 ctrl;

	if (sport->lpuart_dma_tx_use) {
		if (!lpuart_stopped_or_empty(port))
			lpuart_dma_tx(sport);
	} else {
		ctrl = lpuart32_read(port, UARTCTRL);
		lpuart32_write(port, ctrl | UARTCTRL_TIE, UARTCTRL);

		if (lpuart32_read(port, UARTSTAT) & UARTSTAT_TDRE)
			lpuart32_transmit_buffer(sport);
	}
}

static void
lpuart_uart_pm(struct uart_port *port, unsigned int state, unsigned int oldstate)
{
	switch (state) {
	case UART_PM_STATE_OFF:
		pm_runtime_mark_last_busy(port->dev);
		pm_runtime_put_autosuspend(port->dev);
		break;
	default:
		pm_runtime_get_sync(port->dev);
		break;
	}
}

/* return TIOCSER_TEMT when transmitter is not busy */
static unsigned int lpuart_tx_empty(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port,
			struct lpuart_port, port);
	u8 sr1 = readb(port->membase + UARTSR1);
	u8 sfifo = readb(port->membase + UARTSFIFO);

	if (sport->dma_tx_in_progress)
		return 0;

	if (sr1 & UARTSR1_TC && sfifo & UARTSFIFO_TXEMPT)
		return TIOCSER_TEMT;

	return 0;
}

static unsigned int lpuart32_tx_empty(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port,
			struct lpuart_port, port);
	u32 stat = lpuart32_read(port, UARTSTAT);
	u32 sfifo = lpuart32_read(port, UARTFIFO);
	u32 ctrl = lpuart32_read(port, UARTCTRL);

	if (sport->dma_tx_in_progress)
		return 0;

	/*
	 * LPUART Transmission Complete Flag may never be set while queuing a break
	 * character, so avoid checking for transmission complete when UARTCTRL_SBK
	 * is asserted.
	 */
	if ((stat & UARTSTAT_TC && sfifo & UARTFIFO_TXEMPT) || ctrl & UARTCTRL_SBK)
		return TIOCSER_TEMT;

	return 0;
}

static void lpuart_txint(struct lpuart_port *sport)
{
	uart_port_lock(&sport->port);
	lpuart_transmit_buffer(sport);
	uart_port_unlock(&sport->port);
}

static void lpuart_rxint(struct lpuart_port *sport)
{
	unsigned int flg, ignored = 0, overrun = 0;
	struct tty_port *port = &sport->port.state->port;
	u8 rx, sr;

	uart_port_lock(&sport->port);

	while (!(readb(sport->port.membase + UARTSFIFO) & UARTSFIFO_RXEMPT)) {
		flg = TTY_NORMAL;
		sport->port.icount.rx++;
		/*
		 * to clear the FE, OR, NF, FE, PE flags,
		 * read SR1 then read DR
		 */
		sr = readb(sport->port.membase + UARTSR1);
		rx = readb(sport->port.membase + UARTDR);

		if (uart_prepare_sysrq_char(&sport->port, rx))
			continue;

		if (sr & (UARTSR1_PE | UARTSR1_OR | UARTSR1_FE)) {
			if (sr & UARTSR1_PE)
				sport->port.icount.parity++;
			else if (sr & UARTSR1_FE)
				sport->port.icount.frame++;

			if (sr & UARTSR1_OR)
				overrun++;

			if (sr & sport->port.ignore_status_mask) {
				if (++ignored > 100)
					goto out;
				continue;
			}

			sr &= sport->port.read_status_mask;

			if (sr & UARTSR1_PE)
				flg = TTY_PARITY;
			else if (sr & UARTSR1_FE)
				flg = TTY_FRAME;

			if (sr & UARTSR1_OR)
				flg = TTY_OVERRUN;

			sport->port.sysrq = 0;
		}

		if (tty_insert_flip_char(port, rx, flg) == 0)
			sport->port.icount.buf_overrun++;
	}

out:
	if (overrun) {
		sport->port.icount.overrun += overrun;

		/*
		 * Overruns cause FIFO pointers to become missaligned.
		 * Flushing the receive FIFO reinitializes the pointers.
		 */
		writeb(UARTCFIFO_RXFLUSH, sport->port.membase + UARTCFIFO);
		writeb(UARTSFIFO_RXOF, sport->port.membase + UARTSFIFO);
	}

	uart_unlock_and_check_sysrq(&sport->port);

	tty_flip_buffer_push(port);
}

static void lpuart32_txint(struct lpuart_port *sport)
{
	uart_port_lock(&sport->port);
	lpuart32_transmit_buffer(sport);
	uart_port_unlock(&sport->port);
}

static void lpuart32_rxint(struct lpuart_port *sport)
{
	unsigned int flg, ignored = 0;
	struct tty_port *port = &sport->port.state->port;
	u32 rx, sr;
	bool is_break;

	uart_port_lock(&sport->port);

	while (!(lpuart32_read(&sport->port, UARTFIFO) & UARTFIFO_RXEMPT)) {
		flg = TTY_NORMAL;
		sport->port.icount.rx++;
		/*
		 * to clear the FE, OR, NF, FE, PE flags,
		 * read STAT then read DATA reg
		 */
		sr = lpuart32_read(&sport->port, UARTSTAT);
		rx = lpuart32_read(&sport->port, UARTDATA);
		rx &= UARTDATA_MASK;

		/*
		 * The LPUART can't distinguish between a break and a framing error,
		 * thus we assume it is a break if the received data is zero.
		 */
		is_break = (sr & UARTSTAT_FE) && !rx;

		if (is_break && uart_handle_break(&sport->port))
			continue;

		if (uart_prepare_sysrq_char(&sport->port, rx))
			continue;

		if (sr & (UARTSTAT_PE | UARTSTAT_OR | UARTSTAT_FE)) {
			if (sr & UARTSTAT_PE) {
				sport->port.icount.parity++;
			} else if (sr & UARTSTAT_FE) {
				if (is_break)
					sport->port.icount.brk++;
				else
					sport->port.icount.frame++;
			}

			if (sr & UARTSTAT_OR)
				sport->port.icount.overrun++;

			if (sr & sport->port.ignore_status_mask) {
				if (++ignored > 100)
					goto out;
				continue;
			}

			sr &= sport->port.read_status_mask;

			if (sr & UARTSTAT_PE) {
				flg = TTY_PARITY;
			} else if (sr & UARTSTAT_FE) {
				if (is_break)
					flg = TTY_BREAK;
				else
					flg = TTY_FRAME;
			}

			if (sr & UARTSTAT_OR)
				flg = TTY_OVERRUN;
		}

		if (sport->is_cs7)
			rx &= 0x7F;

		if (tty_insert_flip_char(port, rx, flg) == 0)
			sport->port.icount.buf_overrun++;
	}

out:
	uart_unlock_and_check_sysrq(&sport->port);

	tty_flip_buffer_push(port);
}

static irqreturn_t lpuart_int(int irq, void *dev_id)
{
	struct lpuart_port *sport = dev_id;
	u8 sts;

	sts = readb(sport->port.membase + UARTSR1);

	/* SysRq, using dma, check for linebreak by framing err. */
	if (sts & UARTSR1_FE && sport->lpuart_dma_rx_use) {
		readb(sport->port.membase + UARTDR);
		uart_handle_break(&sport->port);
		/* linebreak produces some garbage, removing it */
		writeb(UARTCFIFO_RXFLUSH, sport->port.membase + UARTCFIFO);
		return IRQ_HANDLED;
	}

	if (sts & UARTSR1_RDRF && !sport->lpuart_dma_rx_use)
		lpuart_rxint(sport);

	if (sts & UARTSR1_TDRE && !sport->lpuart_dma_tx_use)
		lpuart_txint(sport);

	return IRQ_HANDLED;
}

static inline void lpuart_handle_sysrq_chars(struct uart_port *port,
					     unsigned char *p, int count)
{
	while (count--) {
		if (*p && uart_handle_sysrq_char(port, *p))
			return;
		p++;
	}
}

static void lpuart_handle_sysrq(struct lpuart_port *sport)
{
	struct circ_buf *ring = &sport->rx_ring;
	int count;

	if (ring->head < ring->tail) {
		count = sport->rx_sgl.length - ring->tail;
		lpuart_handle_sysrq_chars(&sport->port,
					  ring->buf + ring->tail, count);
		ring->tail = 0;
	}

	if (ring->head > ring->tail) {
		count = ring->head - ring->tail;
		lpuart_handle_sysrq_chars(&sport->port,
					  ring->buf + ring->tail, count);
		ring->tail = ring->head;
	}
}

static int lpuart_tty_insert_flip_string(struct tty_port *port,
	unsigned char *chars, size_t size, bool is_cs7)
{
	int i;

	if (is_cs7)
		for (i = 0; i < size; i++)
			chars[i] &= 0x7F;
	return tty_insert_flip_string(port, chars, size);
}

static void lpuart_copy_rx_to_tty(struct lpuart_port *sport)
{
	struct tty_port *port = &sport->port.state->port;
	struct dma_tx_state state;
	enum dma_status dmastat;
	struct dma_chan *chan = sport->dma_rx_chan;
	struct circ_buf *ring = &sport->rx_ring;
	unsigned long flags;
	int count, copied;

	if (lpuart_is_32(sport)) {
		u32 sr = lpuart32_read(&sport->port, UARTSTAT);

		if (sr & (UARTSTAT_PE | UARTSTAT_FE)) {
			/* Clear the error flags */
			lpuart32_write(&sport->port, sr, UARTSTAT);

			if (sr & UARTSTAT_PE)
				sport->port.icount.parity++;
			else if (sr & UARTSTAT_FE)
				sport->port.icount.frame++;
		}
	} else {
		u8 sr = readb(sport->port.membase + UARTSR1);

		if (sr & (UARTSR1_PE | UARTSR1_FE)) {
			u8 cr2;

			/* Disable receiver during this operation... */
			cr2 = readb(sport->port.membase + UARTCR2);
			cr2 &= ~UARTCR2_RE;
			writeb(cr2, sport->port.membase + UARTCR2);

			/* Read DR to clear the error flags */
			readb(sport->port.membase + UARTDR);

			if (sr & UARTSR1_PE)
				sport->port.icount.parity++;
			else if (sr & UARTSR1_FE)
				sport->port.icount.frame++;
			/*
			 * At this point parity/framing error is
			 * cleared However, since the DMA already read
			 * the data register and we had to read it
			 * again after reading the status register to
			 * properly clear the flags, the FIFO actually
			 * underflowed... This requires a clearing of
			 * the FIFO...
			 */
			if (readb(sport->port.membase + UARTSFIFO) &
			    UARTSFIFO_RXUF) {
				writeb(UARTSFIFO_RXUF,
				       sport->port.membase + UARTSFIFO);
				writeb(UARTCFIFO_RXFLUSH,
				       sport->port.membase + UARTCFIFO);
			}

			cr2 |= UARTCR2_RE;
			writeb(cr2, sport->port.membase + UARTCR2);
		}
	}

	async_tx_ack(sport->dma_rx_desc);

	uart_port_lock_irqsave(&sport->port, &flags);

	dmastat = dmaengine_tx_status(chan, sport->dma_rx_cookie, &state);
	if (dmastat == DMA_ERROR) {
		dev_err(sport->port.dev, "Rx DMA transfer failed!\n");
		uart_port_unlock_irqrestore(&sport->port, flags);
		return;
	}

	/* CPU claims ownership of RX DMA buffer */
	dma_sync_sg_for_cpu(chan->device->dev, &sport->rx_sgl, 1,
			    DMA_FROM_DEVICE);

	/*
	 * ring->head points to the end of data already written by the DMA.
	 * ring->tail points to the beginning of data to be read by the
	 * framework.
	 * The current transfer size should not be larger than the dma buffer
	 * length.
	 */
	ring->head = sport->rx_sgl.length - state.residue;
	BUG_ON(ring->head > sport->rx_sgl.length);

	/*
	 * Silent handling of keys pressed in the sysrq timeframe
	 */
	if (sport->port.sysrq) {
		lpuart_handle_sysrq(sport);
		goto exit;
	}

	/*
	 * At this point ring->head may point to the first byte right after the
	 * last byte of the dma buffer:
	 * 0 <= ring->head <= sport->rx_sgl.length
	 *
	 * However ring->tail must always points inside the dma buffer:
	 * 0 <= ring->tail <= sport->rx_sgl.length - 1
	 *
	 * Since we use a ring buffer, we have to handle the case
	 * where head is lower than tail. In such a case, we first read from
	 * tail to the end of the buffer then reset tail.
	 */
	if (ring->head < ring->tail) {
		count = sport->rx_sgl.length - ring->tail;

		copied = lpuart_tty_insert_flip_string(port, ring->buf + ring->tail,
					count, sport->is_cs7);
		if (copied != count)
			sport->port.icount.buf_overrun++;
		ring->tail = 0;
		sport->port.icount.rx += copied;
	}

	/* Finally we read data from tail to head */
	if (ring->tail < ring->head) {
		count = ring->head - ring->tail;
		copied = lpuart_tty_insert_flip_string(port, ring->buf + ring->tail,
					count, sport->is_cs7);
		if (copied != count)
			sport->port.icount.buf_overrun++;
		/* Wrap ring->head if needed */
		if (ring->head >= sport->rx_sgl.length)
			ring->head = 0;
		ring->tail = ring->head;
		sport->port.icount.rx += copied;
	}

	sport->last_residue = state.residue;

exit:
	dma_sync_sg_for_device(chan->device->dev, &sport->rx_sgl, 1,
			       DMA_FROM_DEVICE);

	uart_port_unlock_irqrestore(&sport->port, flags);

	tty_flip_buffer_push(port);
	if (!sport->dma_idle_int)
		mod_timer(&sport->lpuart_timer, jiffies + sport->dma_rx_timeout);
}

static void lpuart_dma_rx_complete(void *arg)
{
	struct lpuart_port *sport = arg;

	lpuart_copy_rx_to_tty(sport);
}

static void lpuart32_dma_idleint(struct lpuart_port *sport)
{
	enum dma_status dmastat;
	struct dma_chan *chan = sport->dma_rx_chan;
	struct circ_buf *ring = &sport->rx_ring;
	struct dma_tx_state state;
	int count = 0;

	dmastat = dmaengine_tx_status(chan, sport->dma_rx_cookie, &state);
	if (dmastat == DMA_ERROR) {
		dev_err(sport->port.dev, "Rx DMA transfer failed!\n");
		return;
	}

	ring->head = sport->rx_sgl.length - state.residue;
	count = CIRC_CNT(ring->head, ring->tail, sport->rx_sgl.length);

	/* Check if new data received before copying */
	if (count)
		lpuart_copy_rx_to_tty(sport);
}

static irqreturn_t lpuart32_int(int irq, void *dev_id)
{
	struct lpuart_port *sport = dev_id;
	u32 sts, rxcount;

	sts = lpuart32_read(&sport->port, UARTSTAT);
	rxcount = lpuart32_read(&sport->port, UARTWATER);
	rxcount = rxcount >> UARTWATER_RXCNT_OFF;

	if ((sts & UARTSTAT_RDRF || rxcount > 0) && !sport->lpuart_dma_rx_use)
		lpuart32_rxint(sport);

	if ((sts & UARTSTAT_TDRE) && !sport->lpuart_dma_tx_use)
		lpuart32_txint(sport);

	if ((sts & UARTSTAT_IDLE) && sport->lpuart_dma_rx_use && sport->dma_idle_int)
		lpuart32_dma_idleint(sport);

	lpuart32_write(&sport->port, sts, UARTSTAT);
	return IRQ_HANDLED;
}

/*
 * Timer function to simulate the hardware EOP (End Of Package) event.
 * The timer callback is to check for new RX data and copy to TTY buffer.
 * If no new data are received since last interval, the EOP condition is
 * met, complete the DMA transfer by copying the data. Otherwise, just
 * restart timer.
 */
static void lpuart_timer_func(struct timer_list *t)
{
	struct lpuart_port *sport = timer_container_of(sport, t, lpuart_timer);
	enum dma_status dmastat;
	struct dma_chan *chan = sport->dma_rx_chan;
	struct circ_buf *ring = &sport->rx_ring;
	struct dma_tx_state state;
	unsigned long flags;
	int count;

	dmastat = dmaengine_tx_status(chan, sport->dma_rx_cookie, &state);
	if (dmastat == DMA_ERROR) {
		dev_err(sport->port.dev, "Rx DMA transfer failed!\n");
		return;
	}

	ring->head = sport->rx_sgl.length - state.residue;
	count = CIRC_CNT(ring->head, ring->tail, sport->rx_sgl.length);

	/* Check if new data received before copying */
	if ((count != 0) && (sport->last_residue == state.residue))
		lpuart_copy_rx_to_tty(sport);
	else
		mod_timer(&sport->lpuart_timer,
			  jiffies + sport->dma_rx_timeout);

	if (uart_port_trylock_irqsave(&sport->port, &flags)) {
		sport->last_residue = state.residue;
		uart_port_unlock_irqrestore(&sport->port, flags);
	}
}

static inline int lpuart_start_rx_dma(struct lpuart_port *sport)
{
	struct dma_slave_config dma_rx_sconfig = {};
	struct circ_buf *ring = &sport->rx_ring;
	int ret, nent;
	struct tty_port *port = &sport->port.state->port;
	struct tty_struct *tty = port->tty;
	struct ktermios *termios = &tty->termios;
	struct dma_chan *chan = sport->dma_rx_chan;
	unsigned int bits = tty_get_frame_size(termios->c_cflag);
	unsigned int baud = tty_get_baud_rate(tty);

	/*
	 * Calculate length of one DMA buffer size to keep latency below
	 * 10ms at any baud rate.
	 */
	sport->rx_dma_rng_buf_len = (DMA_RX_TIMEOUT * baud /  bits / 1000) * 2;
	sport->rx_dma_rng_buf_len = (1 << fls(sport->rx_dma_rng_buf_len));
	sport->rx_dma_rng_buf_len = max_t(int,
					  sport->rxfifo_size * 2,
					  sport->rx_dma_rng_buf_len);
	/*
	 * Keep this condition check in case rxfifo_size is unavailable
	 * for some SoCs.
	 */
	if (sport->rx_dma_rng_buf_len < 16)
		sport->rx_dma_rng_buf_len = 16;

	sport->last_residue = 0;
	sport->dma_rx_timeout = max(nsecs_to_jiffies(
		sport->port.frame_time * DMA_RX_IDLE_CHARS), 1UL);

	ring->buf = kzalloc(sport->rx_dma_rng_buf_len, GFP_ATOMIC);
	if (!ring->buf)
		return -ENOMEM;

	sg_init_one(&sport->rx_sgl, ring->buf, sport->rx_dma_rng_buf_len);
	nent = dma_map_sg(chan->device->dev, &sport->rx_sgl, 1,
			  DMA_FROM_DEVICE);

	if (!nent) {
		dev_err(sport->port.dev, "DMA Rx mapping error\n");
		return -EINVAL;
	}

	dma_rx_sconfig.src_addr = lpuart_dma_datareg_addr(sport);
	dma_rx_sconfig.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	dma_rx_sconfig.src_maxburst = 1;
	dma_rx_sconfig.direction = DMA_DEV_TO_MEM;
	ret = dmaengine_slave_config(chan, &dma_rx_sconfig);

	if (ret < 0) {
		dev_err(sport->port.dev,
				"DMA Rx slave config failed, err = %d\n", ret);
		return ret;
	}

	sport->dma_rx_desc = dmaengine_prep_dma_cyclic(chan,
				 sg_dma_address(&sport->rx_sgl),
				 sport->rx_sgl.length,
				 sport->rx_sgl.length / 2,
				 DMA_DEV_TO_MEM,
				 DMA_PREP_INTERRUPT);
	if (!sport->dma_rx_desc) {
		dev_err(sport->port.dev, "Cannot prepare cyclic DMA\n");
		return -EFAULT;
	}

	sport->dma_rx_desc->callback = lpuart_dma_rx_complete;
	sport->dma_rx_desc->callback_param = sport;
	sport->dma_rx_cookie = dmaengine_submit(sport->dma_rx_desc);
	dma_async_issue_pending(chan);

	if (lpuart_is_32(sport)) {
		u32 baud = lpuart32_read(&sport->port, UARTBAUD);

		lpuart32_write(&sport->port, baud | UARTBAUD_RDMAE, UARTBAUD);

		if (sport->dma_idle_int) {
			u32 ctrl = lpuart32_read(&sport->port, UARTCTRL);

			lpuart32_write(&sport->port, ctrl | UARTCTRL_ILIE, UARTCTRL);
		}
	} else {
		writeb(readb(sport->port.membase + UARTCR5) | UARTCR5_RDMAS,
		       sport->port.membase + UARTCR5);
	}

	return 0;
}

static void lpuart_dma_rx_free(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port,
					struct lpuart_port, port);
	struct dma_chan *chan = sport->dma_rx_chan;

	dmaengine_terminate_sync(chan);
	if (!sport->dma_idle_int)
		timer_delete_sync(&sport->lpuart_timer);

	dma_unmap_sg(chan->device->dev, &sport->rx_sgl, 1, DMA_FROM_DEVICE);
	kfree(sport->rx_ring.buf);
	sport->rx_ring.tail = 0;
	sport->rx_ring.head = 0;
	sport->dma_rx_desc = NULL;
	sport->dma_rx_cookie = -EINVAL;
}

static int lpuart_config_rs485(struct uart_port *port, struct ktermios *termios,
			struct serial_rs485 *rs485)
{
	u8 modem = readb(port->membase + UARTMODEM) &
		~(UARTMODEM_TXRTSPOL | UARTMODEM_TXRTSE);
	writeb(modem, port->membase + UARTMODEM);

	if (rs485->flags & SER_RS485_ENABLED) {
		/* Enable auto RS-485 RTS mode */
		modem |= UARTMODEM_TXRTSE;

		/*
		 * The hardware defaults to RTS logic HIGH while transfer.
		 * Switch polarity in case RTS shall be logic HIGH
		 * after transfer.
		 * Note: UART is assumed to be active high.
		 */
		if (rs485->flags & SER_RS485_RTS_ON_SEND)
			modem |= UARTMODEM_TXRTSPOL;
		else if (rs485->flags & SER_RS485_RTS_AFTER_SEND)
			modem &= ~UARTMODEM_TXRTSPOL;
	}

	writeb(modem, port->membase + UARTMODEM);
	return 0;
}

static int lpuart32_config_rs485(struct uart_port *port, struct ktermios *termios,
			struct serial_rs485 *rs485)
{
	u32 modem = lpuart32_read(port, UARTMODIR)
				& ~(UARTMODIR_TXRTSPOL | UARTMODIR_TXRTSE);
	u32 ctrl;

	/* TXRTSE and TXRTSPOL only can be changed when transmitter is disabled. */
	ctrl = lpuart32_read(port, UARTCTRL);
	if (ctrl & UARTCTRL_TE) {
		/* wait for the transmit engine to complete */
		lpuart32_wait_bit_set(port, UARTSTAT, UARTSTAT_TC);
		lpuart32_write(port, ctrl & ~UARTCTRL_TE, UARTCTRL);

		while (lpuart32_read(port, UARTCTRL) & UARTCTRL_TE)
			cpu_relax();
	}

	lpuart32_write(port, modem, UARTMODIR);

	if (rs485->flags & SER_RS485_ENABLED) {
		/* Enable auto RS-485 RTS mode */
		modem |= UARTMODIR_TXRTSE;

		/*
		 * The hardware defaults to RTS logic HIGH while transfer.
		 * Switch polarity in case RTS shall be logic HIGH
		 * after transfer.
		 * Note: UART is assumed to be active high.
		 */
		if (rs485->flags & SER_RS485_RTS_ON_SEND)
			modem |= UARTMODIR_TXRTSPOL;
		else if (rs485->flags & SER_RS485_RTS_AFTER_SEND)
			modem &= ~UARTMODIR_TXRTSPOL;
	}

	lpuart32_write(port, modem, UARTMODIR);

	if (ctrl & UARTCTRL_TE)
		lpuart32_write(port, ctrl, UARTCTRL);

	return 0;
}

static unsigned int lpuart_get_mctrl(struct uart_port *port)
{
	unsigned int mctrl = 0;
	u8 cr1;

	cr1 = readb(port->membase + UARTCR1);
	if (cr1 & UARTCR1_LOOPS)
		mctrl |= TIOCM_LOOP;

	return mctrl;
}

static unsigned int lpuart32_get_mctrl(struct uart_port *port)
{
	unsigned int mctrl = TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
	u32 ctrl;

	ctrl = lpuart32_read(port, UARTCTRL);
	if (ctrl & UARTCTRL_LOOPS)
		mctrl |= TIOCM_LOOP;

	return mctrl;
}

static void lpuart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	u8 cr1;

	cr1 = readb(port->membase + UARTCR1);

	/* for internal loopback we need LOOPS=1 and RSRC=0 */
	cr1 &= ~(UARTCR1_LOOPS | UARTCR1_RSRC);
	if (mctrl & TIOCM_LOOP)
		cr1 |= UARTCR1_LOOPS;

	writeb(cr1, port->membase + UARTCR1);
}

static void lpuart32_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	u32 ctrl;

	ctrl = lpuart32_read(port, UARTCTRL);

	/* for internal loopback we need LOOPS=1 and RSRC=0 */
	ctrl &= ~(UARTCTRL_LOOPS | UARTCTRL_RSRC);
	if (mctrl & TIOCM_LOOP)
		ctrl |= UARTCTRL_LOOPS;

	lpuart32_write(port, ctrl, UARTCTRL);
}

static void lpuart_break_ctl(struct uart_port *port, int break_state)
{
	u8 cr2;

	cr2 = readb(port->membase + UARTCR2) & ~UARTCR2_SBK;

	if (break_state != 0)
		cr2 |= UARTCR2_SBK;

	writeb(cr2, port->membase + UARTCR2);
}

static void lpuart32_break_ctl(struct uart_port *port, int break_state)
{
	u32 ctrl;

	ctrl = lpuart32_read(port, UARTCTRL);

	/*
	 * LPUART IP now has two known bugs, one is CTS has higher priority than the
	 * break signal, which causes the break signal sending through UARTCTRL_SBK
	 * may impacted by the CTS input if the HW flow control is enabled. It
	 * exists on all platforms we support in this driver.
	 * Another bug is i.MX8QM LPUART may have an additional break character
	 * being sent after SBK was cleared.
	 * To avoid above two bugs, we use Transmit Data Inversion function to send
	 * the break signal instead of UARTCTRL_SBK.
	 */
	if (break_state != 0) {
		/*
		 * Disable the transmitter to prevent any data from being sent out
		 * during break, then invert the TX line to send break.
		 */
		ctrl &= ~UARTCTRL_TE;
		lpuart32_write(port, ctrl, UARTCTRL);
		ctrl |= UARTCTRL_TXINV;
		lpuart32_write(port, ctrl, UARTCTRL);
	} else {
		/* Disable the TXINV to turn off break and re-enable transmitter. */
		ctrl &= ~UARTCTRL_TXINV;
		lpuart32_write(port, ctrl, UARTCTRL);
		ctrl |= UARTCTRL_TE;
		lpuart32_write(port, ctrl, UARTCTRL);
	}
}

static void lpuart_setup_watermark(struct lpuart_port *sport)
{
	u8 fifo, cr2, cr2_saved;

	cr2 = readb(sport->port.membase + UARTCR2);
	cr2_saved = cr2;
	cr2 &= ~(UARTCR2_TIE | UARTCR2_TCIE | UARTCR2_TE |
			UARTCR2_RIE | UARTCR2_RE);
	writeb(cr2, sport->port.membase + UARTCR2);

	fifo = readb(sport->port.membase + UARTPFIFO);
	writeb(fifo | UARTPFIFO_TXFE | UARTPFIFO_RXFE,
			sport->port.membase + UARTPFIFO);

	/* flush Tx and Rx FIFO */
	writeb(UARTCFIFO_TXFLUSH | UARTCFIFO_RXFLUSH,
			sport->port.membase + UARTCFIFO);

	/* explicitly clear RDRF */
	if (readb(sport->port.membase + UARTSR1) & UARTSR1_RDRF) {
		readb(sport->port.membase + UARTDR);
		writeb(UARTSFIFO_RXUF, sport->port.membase + UARTSFIFO);
	}

	if (uart_console(&sport->port))
		sport->rx_watermark = 1;
	writeb(0, sport->port.membase + UARTTWFIFO);
	writeb(sport->rx_watermark, sport->port.membase + UARTRWFIFO);

	/* Restore cr2 */
	writeb(cr2_saved, sport->port.membase + UARTCR2);
}

static void lpuart_setup_watermark_enable(struct lpuart_port *sport)
{
	u8 cr2;

	lpuart_setup_watermark(sport);

	cr2 = readb(sport->port.membase + UARTCR2);
	cr2 |= UARTCR2_RIE | UARTCR2_RE | UARTCR2_TE;
	writeb(cr2, sport->port.membase + UARTCR2);
}

static void lpuart32_setup_watermark(struct lpuart_port *sport)
{
	u32 val, ctrl, ctrl_saved;

	ctrl = lpuart32_read(&sport->port, UARTCTRL);
	ctrl_saved = ctrl;
	ctrl &= ~(UARTCTRL_TIE | UARTCTRL_TCIE | UARTCTRL_TE |
			UARTCTRL_RIE | UARTCTRL_RE | UARTCTRL_ILIE);
	lpuart32_write(&sport->port, ctrl, UARTCTRL);

	/* enable FIFO mode */
	val = lpuart32_read(&sport->port, UARTFIFO);
	val |= UARTFIFO_TXFE | UARTFIFO_RXFE;
	val |= UARTFIFO_TXFLUSH | UARTFIFO_RXFLUSH;
	val |= FIELD_PREP(UARTFIFO_RXIDEN, 0x3);
	lpuart32_write(&sport->port, val, UARTFIFO);

	/* set the watermark */
	if (uart_console(&sport->port))
		sport->rx_watermark = 1;
	val = (sport->rx_watermark << UARTWATER_RXWATER_OFF) |
	      (0x0 << UARTWATER_TXWATER_OFF);
	lpuart32_write(&sport->port, val, UARTWATER);

	/* set RTS watermark */
	if (!uart_console(&sport->port)) {
		val = lpuart32_read(&sport->port, UARTMODIR);
		val |= FIELD_PREP(UARTMODIR_RTSWATER, sport->rxfifo_size >> 1);
		lpuart32_write(&sport->port, val, UARTMODIR);
	}

	/* Restore cr2 */
	lpuart32_write(&sport->port, ctrl_saved, UARTCTRL);
}

static void lpuart32_setup_watermark_enable(struct lpuart_port *sport)
{
	u32 ctrl;

	lpuart32_setup_watermark(sport);

	ctrl = lpuart32_read(&sport->port, UARTCTRL);
	ctrl |= UARTCTRL_RE | UARTCTRL_TE;
	ctrl |= FIELD_PREP(UARTCTRL_IDLECFG, 0x7);
	lpuart32_write(&sport->port, ctrl, UARTCTRL);
}

static void rx_dma_timer_init(struct lpuart_port *sport)
{
	if (sport->dma_idle_int)
		return;

	timer_setup(&sport->lpuart_timer, lpuart_timer_func, 0);
	sport->lpuart_timer.expires = jiffies + sport->dma_rx_timeout;
	add_timer(&sport->lpuart_timer);
}

static void lpuart_request_dma(struct lpuart_port *sport)
{
	sport->dma_tx_chan = dma_request_chan(sport->port.dev, "tx");
	if (IS_ERR(sport->dma_tx_chan)) {
		dev_dbg_once(sport->port.dev,
			     "DMA tx channel request failed, operating without tx DMA (%ld)\n",
			     PTR_ERR(sport->dma_tx_chan));
		sport->dma_tx_chan = NULL;
	}

	sport->dma_rx_chan = dma_request_chan(sport->port.dev, "rx");
	if (IS_ERR(sport->dma_rx_chan)) {
		dev_dbg_once(sport->port.dev,
			     "DMA rx channel request failed, operating without rx DMA (%ld)\n",
			     PTR_ERR(sport->dma_rx_chan));
		sport->dma_rx_chan = NULL;
	}
}

static void lpuart_tx_dma_startup(struct lpuart_port *sport)
{
	u32 uartbaud;
	int ret;

	if (uart_console(&sport->port))
		goto err;

	if (!sport->dma_tx_chan)
		goto err;

	ret = lpuart_dma_tx_request(&sport->port);
	if (ret)
		goto err;

	init_waitqueue_head(&sport->dma_wait);
	sport->lpuart_dma_tx_use = true;
	if (lpuart_is_32(sport)) {
		uartbaud = lpuart32_read(&sport->port, UARTBAUD);
		lpuart32_write(&sport->port,
			       uartbaud | UARTBAUD_TDMAE, UARTBAUD);
	} else {
		writeb(readb(sport->port.membase + UARTCR5) |
		       UARTCR5_TDMAS, sport->port.membase + UARTCR5);
	}

	return;

err:
	sport->lpuart_dma_tx_use = false;
}

static void lpuart_rx_dma_startup(struct lpuart_port *sport)
{
	int ret;
	u8 cr3;

	if (uart_console(&sport->port))
		goto err;

	if (!sport->dma_rx_chan)
		goto err;

	/* set default Rx DMA timeout */
	sport->dma_rx_timeout = msecs_to_jiffies(DMA_RX_TIMEOUT);

	ret = lpuart_start_rx_dma(sport);
	if (ret)
		goto err;

	if (!sport->dma_rx_timeout)
		sport->dma_rx_timeout = 1;

	sport->lpuart_dma_rx_use = true;
	rx_dma_timer_init(sport);

	if (sport->port.has_sysrq && !lpuart_is_32(sport)) {
		cr3 = readb(sport->port.membase + UARTCR3);
		cr3 |= UARTCR3_FEIE;
		writeb(cr3, sport->port.membase + UARTCR3);
	}

	return;

err:
	sport->lpuart_dma_rx_use = false;
}

static void lpuart_hw_setup(struct lpuart_port *sport)
{
	unsigned long flags;

	uart_port_lock_irqsave(&sport->port, &flags);

	lpuart_setup_watermark_enable(sport);

	lpuart_rx_dma_startup(sport);
	lpuart_tx_dma_startup(sport);

	uart_port_unlock_irqrestore(&sport->port, flags);
}

static int lpuart_startup(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port, struct lpuart_port, port);
	u8 fifo;

	/* determine FIFO size and enable FIFO mode */
	fifo = readb(port->membase + UARTPFIFO);

	sport->txfifo_size = UARTFIFO_DEPTH((fifo >> UARTPFIFO_TXSIZE_OFF) &
					    UARTPFIFO_FIFOSIZE_MASK);
	port->fifosize = sport->txfifo_size;

	sport->rxfifo_size = UARTFIFO_DEPTH((fifo >> UARTPFIFO_RXSIZE_OFF) &
					    UARTPFIFO_FIFOSIZE_MASK);

	lpuart_request_dma(sport);
	lpuart_hw_setup(sport);

	return 0;
}

static void lpuart32_hw_disable(struct lpuart_port *sport)
{
	u32 ctrl;

	ctrl = lpuart32_read(&sport->port, UARTCTRL);
	ctrl &= ~(UARTCTRL_RIE | UARTCTRL_ILIE | UARTCTRL_RE |
		  UARTCTRL_TIE | UARTCTRL_TE);
	lpuart32_write(&sport->port, ctrl, UARTCTRL);
}

static void lpuart32_configure(struct lpuart_port *sport)
{
	u32 ctrl;

	ctrl = lpuart32_read(&sport->port, UARTCTRL);
	if (!sport->lpuart_dma_rx_use)
		ctrl |= UARTCTRL_RIE | UARTCTRL_ILIE;
	if (!sport->lpuart_dma_tx_use)
		ctrl |= UARTCTRL_TIE;
	lpuart32_write(&sport->port, ctrl, UARTCTRL);
}

static void lpuart32_hw_setup(struct lpuart_port *sport)
{
	unsigned long flags;

	uart_port_lock_irqsave(&sport->port, &flags);

	lpuart32_hw_disable(sport);

	lpuart_rx_dma_startup(sport);
	lpuart_tx_dma_startup(sport);

	lpuart32_setup_watermark_enable(sport);
	lpuart32_configure(sport);

	uart_port_unlock_irqrestore(&sport->port, flags);
}

static int lpuart32_startup(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port, struct lpuart_port, port);
	u32 fifo;

	/* determine FIFO size */
	fifo = lpuart32_read(port, UARTFIFO);

	sport->txfifo_size = UARTFIFO_DEPTH((fifo >> UARTFIFO_TXSIZE_OFF) &
					    UARTFIFO_FIFOSIZE_MASK);
	port->fifosize = sport->txfifo_size;

	sport->rxfifo_size = UARTFIFO_DEPTH((fifo >> UARTFIFO_RXSIZE_OFF) &
					    UARTFIFO_FIFOSIZE_MASK);

	/*
	 * The LS1021A and LS1028A have a fixed FIFO depth of 16 words.
	 * Although they support the RX/TXSIZE fields, their encoding is
	 * different. Eg the reference manual states 0b101 is 16 words.
	 */
	if (is_layerscape_lpuart(sport)) {
		sport->rxfifo_size = 16;
		sport->txfifo_size = 16;
		port->fifosize = sport->txfifo_size;
	}

	lpuart_request_dma(sport);
	lpuart32_hw_setup(sport);

	return 0;
}

static void lpuart_dma_shutdown(struct lpuart_port *sport)
{
	if (sport->lpuart_dma_rx_use) {
		lpuart_dma_rx_free(&sport->port);
		sport->lpuart_dma_rx_use = false;
	}

	if (sport->lpuart_dma_tx_use) {
		if (wait_event_interruptible_timeout(sport->dma_wait,
			!sport->dma_tx_in_progress, msecs_to_jiffies(300)) <= 0) {
			sport->dma_tx_in_progress = false;
			dmaengine_terminate_sync(sport->dma_tx_chan);
		}
		sport->lpuart_dma_tx_use = false;
	}

	if (sport->dma_tx_chan)
		dma_release_channel(sport->dma_tx_chan);
	if (sport->dma_rx_chan)
		dma_release_channel(sport->dma_rx_chan);
}

static void lpuart_shutdown(struct uart_port *port)
{
	struct lpuart_port *sport = container_of(port, struct lpuart_port, port);
	u8 cr2;
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);

	/* disable Rx/Tx and interrupts */
	cr2 = readb(port->membase + UARTCR2);
	cr2 &= ~(UARTCR2_TE | UARTCR2_RE |
			UARTCR2_TIE | UARTCR2_TCIE | UARTCR2_RIE);
	writeb(cr2, port->membase + UARTCR2);

	uart_port_unlock_irqrestore(port, flags);

	lpuart_dma_shutdown(sport);
}

static void lpuart32_shutdown(struct uart_port *port)
{
	struct lpuart_port *sport =
		container_of(port, struct lpuart_port, port);
	u32 temp;
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);

	/* clear status */
	temp = lpuart32_read(port, UARTSTAT);
	lpuart32_write(port, temp, UARTSTAT);

	/* disable Rx/Tx DMA */
	temp = lpuart32_read(port, UARTBAUD);
	temp &= ~(UARTBAUD_TDMAE | UARTBAUD_RDMAE);
	lpuart32_write(port, temp, UARTBAUD);

	/* disable Rx/Tx and interrupts and break condition */
	temp = lpuart32_read(port, UARTCTRL);
	temp &= ~(UARTCTRL_TE | UARTCTRL_RE | UARTCTRL_ILIE |
			UARTCTRL_TIE | UARTCTRL_TCIE | UARTCTRL_RIE | UARTCTRL_SBK);
	lpuart32_write(port, temp, UARTCTRL);

	/* flush Rx/Tx FIFO */
	temp = lpuart32_read(port, UARTFIFO);
	temp |= UARTFIFO_TXFLUSH | UARTFIFO_RXFLUSH;
	lpuart32_write(port, temp, UARTFIFO);

	uart_port_unlock_irqrestore(port, flags);

	lpuart_dma_shutdown(sport);
}

static void
lpuart_set_termios(struct uart_port *port, struct ktermios *termios,
		   const struct ktermios *old)
{
	struct lpuart_port *sport = container_of(port, struct lpuart_port, port);
	unsigned long flags;
	u8 cr1, old_cr1, old_cr2, cr3, cr4, bdh, modem;
	unsigned int  baud;
	unsigned int old_csize = old ? old->c_cflag & CSIZE : CS8;
	unsigned int sbr, brfa;

	cr1 = old_cr1 = readb(port->membase + UARTCR1);
	old_cr2 = readb(port->membase + UARTCR2);
	cr3 = readb(port->membase + UARTCR3);
	cr4 = readb(port->membase + UARTCR4);
	bdh = readb(port->membase + UARTBDH);
	modem = readb(port->membase + UARTMODEM);
	/*
	 * only support CS8 and CS7, and for CS7 must enable PE.
	 * supported mode:
	 *  - (7,e/o,1)
	 *  - (8,n,1)
	 *  - (8,m/s,1)
	 *  - (8,e/o,1)
	 */
	while ((termios->c_cflag & CSIZE) != CS8 &&
		(termios->c_cflag & CSIZE) != CS7) {
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= old_csize;
		old_csize = CS8;
	}

	if ((termios->c_cflag & CSIZE) == CS8 ||
		(termios->c_cflag & CSIZE) == CS7)
		cr1 = old_cr1 & ~UARTCR1_M;

	if (termios->c_cflag & CMSPAR) {
		if ((termios->c_cflag & CSIZE) != CS8) {
			termios->c_cflag &= ~CSIZE;
			termios->c_cflag |= CS8;
		}
		cr1 |= UARTCR1_M;
	}

	/*
	 * When auto RS-485 RTS mode is enabled,
	 * hardware flow control need to be disabled.
	 */
	if (port->rs485.flags & SER_RS485_ENABLED)
		termios->c_cflag &= ~CRTSCTS;

	if (termios->c_cflag & CRTSCTS)
		modem |= UARTMODEM_RXRTSE | UARTMODEM_TXCTSE;
	else
		modem &= ~(UARTMODEM_RXRTSE | UARTMODEM_TXCTSE);

	termios->c_cflag &= ~CSTOPB;

	/* parity must be enabled when CS7 to match 8-bits format */
	if ((termios->c_cflag & CSIZE) == CS7)
		termios->c_cflag |= PARENB;

	if (termios->c_cflag & PARENB) {
		if (termios->c_cflag & CMSPAR) {
			cr1 &= ~UARTCR1_PE;
			if (termios->c_cflag & PARODD)
				cr3 |= UARTCR3_T8;
			else
				cr3 &= ~UARTCR3_T8;
		} else {
			cr1 |= UARTCR1_PE;
			if ((termios->c_cflag & CSIZE) == CS8)
				cr1 |= UARTCR1_M;
			if (termios->c_cflag & PARODD)
				cr1 |= UARTCR1_PT;
			else
				cr1 &= ~UARTCR1_PT;
		}
	} else {
		cr1 &= ~UARTCR1_PE;
	}

	/* ask the core to calculate the divisor */
	baud = uart_get_baud_rate(port, termios, old, 50, port->uartclk / 16);

	/*
	 * Need to update the Ring buffer length according to the selected
	 * baud rate and restart Rx DMA path.
	 *
	 * Since timer function acqures port->lock, need to stop before
	 * acquring same lock because otherwise timer_delete_sync() can deadlock.
	 */
	if (old && sport->lpuart_dma_rx_use)
		lpuart_dma_rx_free(port);

	uart_port_lock_irqsave(port, &flags);

	port->read_status_mask = 0;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= UARTSR1_FE | UARTSR1_PE;
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		port->read_status_mask |= UARTSR1_FE;

	/* characters to ignore */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= UARTSR1_PE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= UARTSR1_FE;
		/*
		 * if we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= UARTSR1_OR;
	}

	/* update the per-port timeout */
	uart_update_timeout(port, termios->c_cflag, baud);

	/* wait transmit engin complete */
	lpuart_wait_bit_set(port, UARTSR1, UARTSR1_TC);

	/* disable transmit and receive */
	writeb(old_cr2 & ~(UARTCR2_TE | UARTCR2_RE),
			port->membase + UARTCR2);

	sbr = port->uartclk / (16 * baud);
	brfa = ((port->uartclk - (16 * sbr * baud)) * 2) / baud;
	bdh &= ~UARTBDH_SBR_MASK;
	bdh |= (sbr >> 8) & 0x1F;
	cr4 &= ~UARTCR4_BRFA_MASK;
	brfa &= UARTCR4_BRFA_MASK;
	writeb(cr4 | brfa, port->membase + UARTCR4);
	writeb(bdh, port->membase + UARTBDH);
	writeb(sbr & 0xFF, port->membase + UARTBDL);
	writeb(cr3, port->membase + UARTCR3);
	writeb(cr1, port->membase + UARTCR1);
	writeb(modem, port->membase + UARTMODEM);

	/* restore control register */
	writeb(old_cr2, port->membase + UARTCR2);

	if (old && sport->lpuart_dma_rx_use) {
		if (!lpuart_start_rx_dma(sport))
			rx_dma_timer_init(sport);
		else
			sport->lpuart_dma_rx_use = false;
	}

	uart_port_unlock_irqrestore(port, flags);
}

static void __lpuart32_serial_setbrg(struct uart_port *port,
				     unsigned int baudrate, bool use_rx_dma,
				     bool use_tx_dma)
{
	u32 sbr, osr, baud_diff, tmp_osr, tmp_sbr, tmp_diff, baud;
	u32 clk = port->uartclk;

	/*
	 * The idea is to use the best OSR (over-sampling rate) possible.
	 * Note, OSR is typically hard-set to 16 in other LPUART instantiations.
	 * Loop to find the best OSR value possible, one that generates minimum
	 * baud_diff iterate through the rest of the supported values of OSR.
	 *
	 * Calculation Formula:
	 *  Baud Rate = baud clock / ((OSR+1) × SBR)
	 */
	baud_diff = baudrate;
	osr = 0;
	sbr = 0;

	for (tmp_osr = 4; tmp_osr <= 32; tmp_osr++) {
		/* calculate the temporary sbr value  */
		tmp_sbr = (clk / (baudrate * tmp_osr));
		if (tmp_sbr == 0)
			tmp_sbr = 1;

		/*
		 * calculate the baud rate difference based on the temporary
		 * osr and sbr values
		 */
		tmp_diff = clk / (tmp_osr * tmp_sbr) - baudrate;

		/* select best values between sbr and sbr+1 */
		baud = clk / (tmp_osr * (tmp_sbr + 1));
		if (tmp_diff > (baudrate - baud)) {
			tmp_diff = baudrate - baud;
			tmp_sbr++;
		}

		if (tmp_sbr > UARTBAUD_SBR_MASK)
			continue;

		if (tmp_diff <= baud_diff) {
			baud_diff = tmp_diff;
			osr = tmp_osr;
			sbr = tmp_sbr;

			if (!baud_diff)
				break;
		}
	}

	/* handle buadrate outside acceptable rate */
	if (baud_diff > ((baudrate / 100) * 3))
		dev_warn(port->dev,
			 "unacceptable baud rate difference of more than 3%%\n");

	baud = lpuart32_read(port, UARTBAUD);

	if ((osr > 3) && (osr < 8))
		baud |= UARTBAUD_BOTHEDGE;

	baud &= ~(UARTBAUD_OSR_MASK << UARTBAUD_OSR_SHIFT);
	baud |= ((osr-1) & UARTBAUD_OSR_MASK) << UARTBAUD_OSR_SHIFT;

	baud &= ~UARTBAUD_SBR_MASK;
	baud |= sbr & UARTBAUD_SBR_MASK;

	if (!use_rx_dma)
		baud &= ~UARTBAUD_RDMAE;
	if (!use_tx_dma)
		baud &= ~UARTBAUD_TDMAE;

	lpuart32_write(port, baud, UARTBAUD);
}

static void lpuart32_serial_setbrg(struct lpuart_port *sport,
				   unsigned int baudrate)
{
	__lpuart32_serial_setbrg(&sport->port, baudrate,
				 sport->lpuart_dma_rx_use,
				 sport->lpuart_dma_tx_use);
}


static void
lpuart32_set_termios(struct uart_port *port, struct ktermios *termios,
		     const struct ktermios *old)
{
	struct lpuart_port *sport = container_of(port, struct lpuart_port, port);
	unsigned long flags;
	u32 ctrl, old_ctrl, bd, modem;
	unsigned int  baud;
	unsigned int old_csize = old ? old->c_cflag & CSIZE : CS8;

	ctrl = old_ctrl = lpuart32_read(port, UARTCTRL);
	bd = lpuart32_read(port, UARTBAUD);
	modem = lpuart32_read(port, UARTMODIR);
	sport->is_cs7 = false;
	/*
	 * only support CS8 and CS7
	 * supported mode:
	 *  - (7,n,1) (imx only)
	 *  - (7,e/o,1)
	 *  - (8,n,1)
	 *  - (8,m/s,1)
	 *  - (8,e/o,1)
	 */
	while ((termios->c_cflag & CSIZE) != CS8 &&
		(termios->c_cflag & CSIZE) != CS7) {
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= old_csize;
		old_csize = CS8;
	}

	if ((termios->c_cflag & CSIZE) == CS8 ||
		(termios->c_cflag & CSIZE) == CS7)
		ctrl = old_ctrl & ~(UARTCTRL_M | UARTCTRL_M7);

	if (termios->c_cflag & CMSPAR) {
		if ((termios->c_cflag & CSIZE) != CS8) {
			termios->c_cflag &= ~CSIZE;
			termios->c_cflag |= CS8;
		}
		ctrl |= UARTCTRL_M;
	}

	/*
	 * When auto RS-485 RTS mode is enabled,
	 * hardware flow control need to be disabled.
	 */
	if (port->rs485.flags & SER_RS485_ENABLED)
		termios->c_cflag &= ~CRTSCTS;

	if (termios->c_cflag & CRTSCTS)
		modem |= UARTMODIR_RXRTSE | UARTMODIR_TXCTSE;
	else
		modem &= ~(UARTMODIR_RXRTSE | UARTMODIR_TXCTSE);

	if (termios->c_cflag & CSTOPB)
		bd |= UARTBAUD_SBNS;
	else
		bd &= ~UARTBAUD_SBNS;

	/*
	 * imx support 7-bits format, no limitation on parity when CS7
	 * for layerscape, parity must be enabled when CS7 to match 8-bits format
	 */
	if ((termios->c_cflag & CSIZE) == CS7 && !(termios->c_cflag & PARENB)) {
		if (is_imx7ulp_lpuart(sport) ||
		    is_imx8ulp_lpuart(sport) ||
		    is_imx8qxp_lpuart(sport))
			ctrl |= UARTCTRL_M7;
		else
			termios->c_cflag |= PARENB;
	}

	if ((termios->c_cflag & PARENB)) {
		if (termios->c_cflag & CMSPAR) {
			ctrl &= ~UARTCTRL_PE;
			ctrl |= UARTCTRL_M;
		} else {
			ctrl |= UARTCTRL_PE;
			if ((termios->c_cflag & CSIZE) == CS8)
				ctrl |= UARTCTRL_M;
			if (termios->c_cflag & PARODD)
				ctrl |= UARTCTRL_PT;
			else
				ctrl &= ~UARTCTRL_PT;
		}
	} else {
		ctrl &= ~UARTCTRL_PE;
	}

	/* ask the core to calculate the divisor */
	baud = uart_get_baud_rate(port, termios, old, 50, port->uartclk / 4);

	/*
	 * Need to update the Ring buffer length according to the selected
	 * baud rate and restart Rx DMA path.
	 *
	 * Since timer function acqures port->lock, need to stop before
	 * acquring same lock because otherwise timer_delete_sync() can deadlock.
	 */
	if (old && sport->lpuart_dma_rx_use)
		lpuart_dma_rx_free(port);

	uart_port_lock_irqsave(port, &flags);

	port->read_status_mask = 0;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= UARTSTAT_FE | UARTSTAT_PE;
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		port->read_status_mask |= UARTSTAT_FE;

	/* characters to ignore */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= UARTSTAT_PE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= UARTSTAT_FE;
		/*
		 * if we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= UARTSTAT_OR;
	}

	/* update the per-port timeout */
	uart_update_timeout(port, termios->c_cflag, baud);

	/*
	 * disable CTS to ensure the transmit engine is not blocked by the flow
	 * control when there is dirty data in TX FIFO
	 */
	lpuart32_write(port, modem & ~UARTMODIR_TXCTSE, UARTMODIR);

	/*
	 * LPUART Transmission Complete Flag may never be set while queuing a break
	 * character, so skip waiting for transmission complete when UARTCTRL_SBK is
	 * asserted.
	 */
	if (!(old_ctrl & UARTCTRL_SBK))
		lpuart32_wait_bit_set(port, UARTSTAT, UARTSTAT_TC);

	/* disable transmit and receive */
	lpuart32_write(port, old_ctrl & ~(UARTCTRL_TE | UARTCTRL_RE),
		       UARTCTRL);

	lpuart32_write(port, bd, UARTBAUD);
	lpuart32_serial_setbrg(sport, baud);
	/* restore control register */
	lpuart32_write(port, ctrl, UARTCTRL);
	/* re-enable the CTS if needed */
	lpuart32_write(port, modem, UARTMODIR);

	if ((ctrl & (UARTCTRL_PE | UARTCTRL_M)) == UARTCTRL_PE)
		sport->is_cs7 = true;

	if (old && sport->lpuart_dma_rx_use) {
		if (!lpuart_start_rx_dma(sport))
			rx_dma_timer_init(sport);
		else
			sport->lpuart_dma_rx_use = false;
	}

	uart_port_unlock_irqrestore(port, flags);
}

static const char *lpuart_type(struct uart_port *port)
{
	return "FSL_LPUART";
}

static void lpuart_release_port(struct uart_port *port)
{
	/* nothing to do */
}

static int lpuart_request_port(struct uart_port *port)
{
	return  0;
}

/* configure/autoconfigure the port */
static void lpuart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_LPUART;
}

static int lpuart_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	int ret = 0;

	if (ser->type != PORT_UNKNOWN && ser->type != PORT_LPUART)
		ret = -EINVAL;
	if (port->irq != ser->irq)
		ret = -EINVAL;
	if (ser->io_type != UPIO_MEM)
		ret = -EINVAL;
	if (port->uartclk / 16 != ser->baud_base)
		ret = -EINVAL;
	if (port->iobase != ser->port)
		ret = -EINVAL;
	if (ser->hub6 != 0)
		ret = -EINVAL;
	return ret;
}

static const struct uart_ops lpuart_pops = {
	.tx_empty	= lpuart_tx_empty,
	.set_mctrl	= lpuart_set_mctrl,
	.get_mctrl	= lpuart_get_mctrl,
	.stop_tx	= lpuart_stop_tx,
	.start_tx	= lpuart_start_tx,
	.stop_rx	= lpuart_stop_rx,
	.break_ctl	= lpuart_break_ctl,
	.startup	= lpuart_startup,
	.shutdown	= lpuart_shutdown,
	.set_termios	= lpuart_set_termios,
	.pm		= lpuart_uart_pm,
	.type		= lpuart_type,
	.request_port	= lpuart_request_port,
	.release_port	= lpuart_release_port,
	.config_port	= lpuart_config_port,
	.verify_port	= lpuart_verify_port,
	.flush_buffer	= lpuart_flush_buffer,
#if defined(CONFIG_CONSOLE_POLL)
	.poll_init	= lpuart_poll_init,
	.poll_get_char	= lpuart_poll_get_char,
	.poll_put_char	= lpuart_poll_put_char,
#endif
};

static const struct uart_ops lpuart32_pops = {
	.tx_empty	= lpuart32_tx_empty,
	.set_mctrl	= lpuart32_set_mctrl,
	.get_mctrl	= lpuart32_get_mctrl,
	.stop_tx	= lpuart32_stop_tx,
	.start_tx	= lpuart32_start_tx,
	.stop_rx	= lpuart32_stop_rx,
	.break_ctl	= lpuart32_break_ctl,
	.startup	= lpuart32_startup,
	.shutdown	= lpuart32_shutdown,
	.set_termios	= lpuart32_set_termios,
	.pm		= lpuart_uart_pm,
	.type		= lpuart_type,
	.request_port	= lpuart_request_port,
	.release_port	= lpuart_release_port,
	.config_port	= lpuart_config_port,
	.verify_port	= lpuart_verify_port,
	.flush_buffer	= lpuart_flush_buffer,
#if defined(CONFIG_CONSOLE_POLL)
	.poll_init	= lpuart32_poll_init,
	.poll_get_char	= lpuart32_poll_get_char,
	.poll_put_char	= lpuart32_poll_put_char,
#endif
};

static struct lpuart_port *lpuart_ports[UART_NR];

#ifdef CONFIG_SERIAL_FSL_LPUART_CONSOLE
static void lpuart_console_putchar(struct uart_port *port, unsigned char ch)
{
	lpuart_wait_bit_set(port, UARTSR1, UARTSR1_TDRE);
	writeb(ch, port->membase + UARTDR);
}

static void lpuart32_console_putchar(struct uart_port *port, unsigned char ch)
{
	lpuart32_wait_bit_set(port, UARTSTAT, UARTSTAT_TDRE);
	lpuart32_write(port, ch, UARTDATA);
}

static void
lpuart_console_write(struct console *co, const char *s, unsigned int count)
{
	struct lpuart_port *sport = lpuart_ports[co->index];
	u8  old_cr2, cr2;
	unsigned long flags;
	int locked = 1;

	if (oops_in_progress)
		locked = uart_port_trylock_irqsave(&sport->port, &flags);
	else
		uart_port_lock_irqsave(&sport->port, &flags);

	/* first save CR2 and then disable interrupts */
	cr2 = old_cr2 = readb(sport->port.membase + UARTCR2);
	cr2 |= UARTCR2_TE | UARTCR2_RE;
	cr2 &= ~(UARTCR2_TIE | UARTCR2_TCIE | UARTCR2_RIE);
	writeb(cr2, sport->port.membase + UARTCR2);

	uart_console_write(&sport->port, s, count, lpuart_console_putchar);

	/* wait for transmitter finish complete and restore CR2 */
	lpuart_wait_bit_set(&sport->port, UARTSR1, UARTSR1_TC);

	writeb(old_cr2, sport->port.membase + UARTCR2);

	if (locked)
		uart_port_unlock_irqrestore(&sport->port, flags);
}

static void
lpuart32_console_write(struct console *co, const char *s, unsigned int count)
{
	struct lpuart_port *sport = lpuart_ports[co->index];
	u32 old_cr, cr;
	unsigned long flags;
	int locked = 1;

	if (oops_in_progress)
		locked = uart_port_trylock_irqsave(&sport->port, &flags);
	else
		uart_port_lock_irqsave(&sport->port, &flags);

	/* first save CR2 and then disable interrupts */
	cr = old_cr = lpuart32_read(&sport->port, UARTCTRL);
	cr |= UARTCTRL_TE | UARTCTRL_RE;
	cr &= ~(UARTCTRL_TIE | UARTCTRL_TCIE | UARTCTRL_RIE);
	lpuart32_write(&sport->port, cr, UARTCTRL);

	uart_console_write(&sport->port, s, count, lpuart32_console_putchar);

	/* wait for transmitter finish complete and restore CR2 */
	lpuart32_wait_bit_set(&sport->port, UARTSTAT, UARTSTAT_TC);

	lpuart32_write(&sport->port, old_cr, UARTCTRL);

	if (locked)
		uart_port_unlock_irqrestore(&sport->port, flags);
}

/*
 * if the port was already initialised (eg, by a boot loader),
 * try to determine the current setup.
 */
static void __init
lpuart_console_get_options(struct lpuart_port *sport, int *baud,
			   int *parity, int *bits)
{
	u8 cr, bdh, bdl, brfa;
	unsigned int sbr, uartclk, baud_raw;

	cr = readb(sport->port.membase + UARTCR2);
	cr &= UARTCR2_TE | UARTCR2_RE;
	if (!cr)
		return;

	/* ok, the port was enabled */

	cr = readb(sport->port.membase + UARTCR1);

	*parity = 'n';
	if (cr & UARTCR1_PE) {
		if (cr & UARTCR1_PT)
			*parity = 'o';
		else
			*parity = 'e';
	}

	if (cr & UARTCR1_M)
		*bits = 9;
	else
		*bits = 8;

	bdh = readb(sport->port.membase + UARTBDH);
	bdh &= UARTBDH_SBR_MASK;
	bdl = readb(sport->port.membase + UARTBDL);
	sbr = bdh;
	sbr <<= 8;
	sbr |= bdl;
	brfa = readb(sport->port.membase + UARTCR4);
	brfa &= UARTCR4_BRFA_MASK;

	uartclk = lpuart_get_baud_clk_rate(sport);
	/*
	 * baud = mod_clk/(16*(sbr[13]+(brfa)/32)
	 */
	baud_raw = uartclk / (16 * (sbr + brfa / 32));

	if (*baud != baud_raw)
		dev_info(sport->port.dev, "Serial: Console lpuart rounded baud rate"
				"from %d to %d\n", baud_raw, *baud);
}

static void __init
lpuart32_console_get_options(struct lpuart_port *sport, int *baud,
			   int *parity, int *bits)
{
	u32 cr, bd;
	unsigned int sbr, uartclk, baud_raw;

	cr = lpuart32_read(&sport->port, UARTCTRL);
	cr &= UARTCTRL_TE | UARTCTRL_RE;
	if (!cr)
		return;

	/* ok, the port was enabled */

	cr = lpuart32_read(&sport->port, UARTCTRL);

	*parity = 'n';
	if (cr & UARTCTRL_PE) {
		if (cr & UARTCTRL_PT)
			*parity = 'o';
		else
			*parity = 'e';
	}

	if (cr & UARTCTRL_M)
		*bits = 9;
	else
		*bits = 8;

	bd = lpuart32_read(&sport->port, UARTBAUD);
	bd &= UARTBAUD_SBR_MASK;
	if (!bd)
		return;

	sbr = bd;
	uartclk = lpuart_get_baud_clk_rate(sport);
	/*
	 * baud = mod_clk/(16*(sbr[13]+(brfa)/32)
	 */
	baud_raw = uartclk / (16 * sbr);

	if (*baud != baud_raw)
		dev_info(sport->port.dev, "Serial: Console lpuart rounded baud rate"
				"from %d to %d\n", baud_raw, *baud);
}

static int __init lpuart_console_setup(struct console *co, char *options)
{
	struct lpuart_port *sport;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	/*
	 * check whether an invalid uart number has been specified, and
	 * if so, search for the first available port that does have
	 * console support.
	 */
	if (co->index == -1 || co->index >= ARRAY_SIZE(lpuart_ports))
		co->index = 0;

	sport = lpuart_ports[co->index];
	if (sport == NULL)
		return -ENODEV;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	else
		if (lpuart_is_32(sport))
			lpuart32_console_get_options(sport, &baud, &parity, &bits);
		else
			lpuart_console_get_options(sport, &baud, &parity, &bits);

	if (lpuart_is_32(sport))
		lpuart32_setup_watermark(sport);
	else
		lpuart_setup_watermark(sport);

	return uart_set_options(&sport->port, co, baud, parity, bits, flow);
}

static struct uart_driver lpuart_reg;
static struct console lpuart_console = {
	.name		= DEV_NAME,
	.write		= lpuart_console_write,
	.device		= uart_console_device,
	.setup		= lpuart_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &lpuart_reg,
};

static struct console lpuart32_console = {
	.name		= DEV_NAME,
	.write		= lpuart32_console_write,
	.device		= uart_console_device,
	.setup		= lpuart_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &lpuart_reg,
};

static void lpuart_early_write(struct console *con, const char *s, unsigned n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, lpuart_console_putchar);
}

static void lpuart32_early_write(struct console *con, const char *s, unsigned n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, lpuart32_console_putchar);
}

static int __init lpuart_early_console_setup(struct earlycon_device *device,
					  const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = lpuart_early_write;
	return 0;
}

static int __init lpuart32_early_console_setup(struct earlycon_device *device,
					  const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	if (device->port.iotype != UPIO_MEM32)
		device->port.iotype = UPIO_MEM32BE;

	device->con->write = lpuart32_early_write;
	return 0;
}

static int __init ls1028a_early_console_setup(struct earlycon_device *device,
					      const char *opt)
{
	u32 cr;

	if (!device->port.membase)
		return -ENODEV;

	device->port.iotype = UPIO_MEM32;
	device->con->write = lpuart32_early_write;

	/* set the baudrate */
	if (device->port.uartclk && device->baud)
		__lpuart32_serial_setbrg(&device->port, device->baud,
					 false, false);

	/* enable transmitter */
	cr = lpuart32_read(&device->port, UARTCTRL);
	cr |= UARTCTRL_TE;
	lpuart32_write(&device->port, cr, UARTCTRL);

	return 0;
}

static int __init lpuart32_imx_early_console_setup(struct earlycon_device *device,
						   const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	device->port.iotype = UPIO_MEM32;
	device->port.membase += IMX_REG_OFF;
	device->con->write = lpuart32_early_write;

	return 0;
}
OF_EARLYCON_DECLARE(lpuart, "fsl,vf610-lpuart", lpuart_early_console_setup);
OF_EARLYCON_DECLARE(lpuart32, "fsl,ls1021a-lpuart", lpuart32_early_console_setup);
OF_EARLYCON_DECLARE(lpuart32, "fsl,ls1028a-lpuart", ls1028a_early_console_setup);
OF_EARLYCON_DECLARE(lpuart32, "fsl,imx7ulp-lpuart", lpuart32_imx_early_console_setup);
OF_EARLYCON_DECLARE(lpuart32, "fsl,imx8ulp-lpuart", lpuart32_imx_early_console_setup);
OF_EARLYCON_DECLARE(lpuart32, "fsl,imx8qxp-lpuart", lpuart32_imx_early_console_setup);
OF_EARLYCON_DECLARE(lpuart32, "fsl,imxrt1050-lpuart", lpuart32_imx_early_console_setup);
EARLYCON_DECLARE(lpuart, lpuart_early_console_setup);
EARLYCON_DECLARE(lpuart32, lpuart32_early_console_setup);

#define LPUART_CONSOLE	(&lpuart_console)
#define LPUART32_CONSOLE	(&lpuart32_console)
#else
#define LPUART_CONSOLE	NULL
#define LPUART32_CONSOLE	NULL
#endif

static struct uart_driver lpuart_reg = {
	.owner		= THIS_MODULE,
	.driver_name	= DRIVER_NAME,
	.dev_name	= DEV_NAME,
	.nr		= ARRAY_SIZE(lpuart_ports),
	.cons		= LPUART_CONSOLE,
};

static const struct serial_rs485 lpuart_rs485_supported = {
	.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND | SER_RS485_RTS_AFTER_SEND,
	/* delay_rts_* and RX_DURING_TX are not supported */
};

static int lpuart_global_reset(struct lpuart_port *sport)
{
	struct uart_port *port = &sport->port;
	void __iomem *global_addr;
	u32 ctrl, bd;
	unsigned int val = 0;
	int ret;

	ret = clk_prepare_enable(sport->ipg_clk);
	if (ret) {
		dev_err(port->dev, "failed to enable uart ipg clk: %d\n", ret);
		return ret;
	}

	if (is_imx7ulp_lpuart(sport) || is_imx8ulp_lpuart(sport) || is_imx8qxp_lpuart(sport)) {
		/*
		 * If the transmitter is used by earlycon, wait for transmit engine to
		 * complete and then reset.
		 */
		ctrl = lpuart32_read(port, UARTCTRL);
		if (ctrl & UARTCTRL_TE) {
			bd = lpuart32_read(port, UARTBAUD);
			if (read_poll_timeout(lpuart32_tx_empty, val, val, 1, 100000, false,
					      port)) {
				dev_warn(port->dev,
					 "timeout waiting for transmit engine to complete\n");
				clk_disable_unprepare(sport->ipg_clk);
				return 0;
			}
		}

		global_addr = port->membase + UART_GLOBAL - IMX_REG_OFF;
		writel(UART_GLOBAL_RST, global_addr);
		usleep_range(GLOBAL_RST_MIN_US, GLOBAL_RST_MAX_US);
		writel(0, global_addr);
		usleep_range(GLOBAL_RST_MIN_US, GLOBAL_RST_MAX_US);

		/* Recover the transmitter for earlycon. */
		if (ctrl & UARTCTRL_TE) {
			lpuart32_write(port, bd, UARTBAUD);
			lpuart32_write(port, ctrl, UARTCTRL);
		}
	}

	clk_disable_unprepare(sport->ipg_clk);
	return 0;
}

static int lpuart_probe(struct platform_device *pdev)
{
	const struct lpuart_soc_data *sdata = of_device_get_match_data(&pdev->dev);
	struct device_node *np = pdev->dev.of_node;
	struct lpuart_port *sport;
	struct resource *res;
	irq_handler_t handler;
	int ret;

	sport = devm_kzalloc(&pdev->dev, sizeof(*sport), GFP_KERNEL);
	if (!sport)
		return -ENOMEM;

	sport->port.membase = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(sport->port.membase))
		return PTR_ERR(sport->port.membase);

	sport->port.membase += sdata->reg_off;
	sport->port.mapbase = res->start + sdata->reg_off;
	sport->port.dev = &pdev->dev;
	sport->port.type = PORT_LPUART;
	sport->devtype = sdata->devtype;
	sport->rx_watermark = sdata->rx_watermark;
	sport->dma_idle_int = is_imx7ulp_lpuart(sport) || is_imx8ulp_lpuart(sport) ||
			      is_imx8qxp_lpuart(sport);
	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	sport->port.irq = ret;
	sport->port.iotype = sdata->iotype;
	if (lpuart_is_32(sport))
		sport->port.ops = &lpuart32_pops;
	else
		sport->port.ops = &lpuart_pops;
	sport->port.has_sysrq = IS_ENABLED(CONFIG_SERIAL_FSL_LPUART_CONSOLE);
	sport->port.flags = UPF_BOOT_AUTOCONF;

	if (lpuart_is_32(sport))
		sport->port.rs485_config = lpuart32_config_rs485;
	else
		sport->port.rs485_config = lpuart_config_rs485;
	sport->port.rs485_supported = lpuart_rs485_supported;

	sport->ipg_clk = devm_clk_get(&pdev->dev, "ipg");
	if (IS_ERR(sport->ipg_clk)) {
		ret = PTR_ERR(sport->ipg_clk);
		return dev_err_probe(&pdev->dev, ret, "failed to get uart ipg clk\n");
	}

	sport->baud_clk = NULL;
	if (is_imx8qxp_lpuart(sport)) {
		sport->baud_clk = devm_clk_get(&pdev->dev, "baud");
		if (IS_ERR(sport->baud_clk)) {
			ret = PTR_ERR(sport->baud_clk);
			return dev_err_probe(&pdev->dev, ret, "failed to get uart baud clk\n");
		}
	}

	ret = of_alias_get_id(np, "serial");
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to get alias id, errno %d\n", ret);
		return ret;
	}
	if (ret >= ARRAY_SIZE(lpuart_ports)) {
		dev_err(&pdev->dev, "serial%d out of range\n", ret);
		return -EINVAL;
	}
	sport->port.line = ret;

	ret = lpuart_enable_clks(sport);
	if (ret)
		return ret;
	sport->port.uartclk = lpuart_get_baud_clk_rate(sport);

	lpuart_ports[sport->port.line] = sport;

	platform_set_drvdata(pdev, &sport->port);

	if (lpuart_is_32(sport)) {
		lpuart_reg.cons = LPUART32_CONSOLE;
		handler = lpuart32_int;
	} else {
		lpuart_reg.cons = LPUART_CONSOLE;
		handler = lpuart_int;
	}

	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&pdev->dev, UART_AUTOSUSPEND_TIMEOUT);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_mark_last_busy(&pdev->dev);

	ret = lpuart_global_reset(sport);
	if (ret)
		goto failed_reset;

	ret = uart_get_rs485_mode(&sport->port);
	if (ret)
		goto failed_get_rs485;

	ret = uart_add_one_port(&lpuart_reg, &sport->port);
	if (ret)
		goto failed_attach_port;

	ret = devm_request_irq(&pdev->dev, sport->port.irq, handler, 0,
			       dev_name(&pdev->dev), sport);
	if (ret)
		goto failed_irq_request;

	return 0;

failed_irq_request:
	uart_remove_one_port(&lpuart_reg, &sport->port);
failed_attach_port:
failed_get_rs485:
failed_reset:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	lpuart_disable_clks(sport);
	return ret;
}

static void lpuart_remove(struct platform_device *pdev)
{
	struct lpuart_port *sport = platform_get_drvdata(pdev);

	uart_remove_one_port(&lpuart_reg, &sport->port);

	lpuart_disable_clks(sport);

	if (sport->dma_tx_chan)
		dma_release_channel(sport->dma_tx_chan);

	if (sport->dma_rx_chan)
		dma_release_channel(sport->dma_rx_chan);

	pm_runtime_disable(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
}

static int lpuart_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct lpuart_port *sport = platform_get_drvdata(pdev);

	lpuart_disable_clks(sport);

	return 0;
};

static int lpuart_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct lpuart_port *sport = platform_get_drvdata(pdev);

	return lpuart_enable_clks(sport);
};

static void serial_lpuart_enable_wakeup(struct lpuart_port *sport, bool on)
{
	u32 val, baud;

	if (lpuart_is_32(sport)) {
		val = lpuart32_read(&sport->port, UARTCTRL);
		baud = lpuart32_read(&sport->port, UARTBAUD);
		if (on) {
			/* set rx_watermark to 0 in wakeup source mode */
			lpuart32_write(&sport->port, 0, UARTWATER);
			val |= UARTCTRL_RIE;
			/* clear RXEDGIF flag before enable RXEDGIE interrupt */
			lpuart32_write(&sport->port, UARTSTAT_RXEDGIF, UARTSTAT);
			baud |= UARTBAUD_RXEDGIE;
		} else {
			val &= ~UARTCTRL_RIE;
			baud &= ~UARTBAUD_RXEDGIE;
		}
		lpuart32_write(&sport->port, val, UARTCTRL);
		lpuart32_write(&sport->port, baud, UARTBAUD);
	} else {
		val = readb(sport->port.membase + UARTCR2);
		if (on)
			val |= UARTCR2_RIE;
		else
			val &= ~UARTCR2_RIE;
		writeb(val, sport->port.membase + UARTCR2);
	}
}

static bool lpuart_uport_is_active(struct lpuart_port *sport)
{
	struct tty_port *port = &sport->port.state->port;
	struct tty_struct *tty;
	struct device *tty_dev;
	int may_wake = 0;

	tty = tty_port_tty_get(port);
	if (tty) {
		tty_dev = tty->dev;
		may_wake = tty_dev && device_may_wakeup(tty_dev);
		tty_kref_put(tty);
	}

	if ((tty_port_initialized(port) && may_wake) ||
	    (!console_suspend_enabled && uart_console(&sport->port)))
		return true;

	return false;
}

static int lpuart_suspend_noirq(struct device *dev)
{
	struct lpuart_port *sport = dev_get_drvdata(dev);
	bool irq_wake = irqd_is_wakeup_set(irq_get_irq_data(sport->port.irq));

	if (lpuart_uport_is_active(sport))
		serial_lpuart_enable_wakeup(sport, !!irq_wake);

	pinctrl_pm_select_sleep_state(dev);

	return 0;
}

static int lpuart_resume_noirq(struct device *dev)
{
	struct lpuart_port *sport = dev_get_drvdata(dev);
	u32 stat;

	pinctrl_pm_select_default_state(dev);

	if (lpuart_uport_is_active(sport)) {
		serial_lpuart_enable_wakeup(sport, false);

		/* clear the wakeup flags */
		if (lpuart_is_32(sport)) {
			stat = lpuart32_read(&sport->port, UARTSTAT);
			lpuart32_write(&sport->port, stat, UARTSTAT);
		}
	}

	return 0;
}

static int lpuart_suspend(struct device *dev)
{
	struct lpuart_port *sport = dev_get_drvdata(dev);
	u32 temp;
	unsigned long flags;

	uart_suspend_port(&lpuart_reg, &sport->port);

	if (lpuart_uport_is_active(sport)) {
		uart_port_lock_irqsave(&sport->port, &flags);
		if (lpuart_is_32(sport)) {
			/* disable Rx/Tx and interrupts */
			temp = lpuart32_read(&sport->port, UARTCTRL);
			temp &= ~(UARTCTRL_TE | UARTCTRL_TIE | UARTCTRL_TCIE);
			lpuart32_write(&sport->port, temp, UARTCTRL);
		} else {
			/* disable Rx/Tx and interrupts */
			temp = readb(sport->port.membase + UARTCR2);
			temp &= ~(UARTCR2_TE | UARTCR2_TIE | UARTCR2_TCIE);
			writeb(temp, sport->port.membase + UARTCR2);
		}
		uart_port_unlock_irqrestore(&sport->port, flags);

		if (sport->lpuart_dma_rx_use) {
			/*
			 * EDMA driver during suspend will forcefully release any
			 * non-idle DMA channels. If port wakeup is enabled or if port
			 * is console port or 'no_console_suspend' is set the Rx DMA
			 * cannot resume as expected, hence gracefully release the
			 * Rx DMA path before suspend and start Rx DMA path on resume.
			 */
			lpuart_dma_rx_free(&sport->port);

			/* Disable Rx DMA to use UART port as wakeup source */
			uart_port_lock_irqsave(&sport->port, &flags);
			if (lpuart_is_32(sport)) {
				temp = lpuart32_read(&sport->port, UARTBAUD);
				lpuart32_write(&sport->port, temp & ~UARTBAUD_RDMAE,
					       UARTBAUD);
			} else {
				writeb(readb(sport->port.membase + UARTCR5) &
				       ~UARTCR5_RDMAS, sport->port.membase + UARTCR5);
			}
			uart_port_unlock_irqrestore(&sport->port, flags);
		}

		if (sport->lpuart_dma_tx_use) {
			uart_port_lock_irqsave(&sport->port, &flags);
			if (lpuart_is_32(sport)) {
				temp = lpuart32_read(&sport->port, UARTBAUD);
				temp &= ~UARTBAUD_TDMAE;
				lpuart32_write(&sport->port, temp, UARTBAUD);
			} else {
				temp = readb(sport->port.membase + UARTCR5);
				temp &= ~UARTCR5_TDMAS;
				writeb(temp, sport->port.membase + UARTCR5);
			}
			uart_port_unlock_irqrestore(&sport->port, flags);
			sport->dma_tx_in_progress = false;
			dmaengine_terminate_sync(sport->dma_tx_chan);
		}
	} else if (pm_runtime_active(sport->port.dev)) {
		lpuart_disable_clks(sport);
		pm_runtime_disable(sport->port.dev);
		pm_runtime_set_suspended(sport->port.dev);
	}

	return 0;
}

static void lpuart_console_fixup(struct lpuart_port *sport)
{
	struct tty_port *port = &sport->port.state->port;
	struct uart_port *uport = &sport->port;
	struct ktermios termios;

	/* i.MX7ULP enter VLLS mode that lpuart module power off and registers
	 * all lost no matter the port is wakeup source.
	 * For console port, console baud rate setting lost and print messy
	 * log when enable the console port as wakeup source. To avoid the
	 * issue happen, user should not enable uart port as wakeup source
	 * in VLLS mode, or restore console setting here.
	 */
	if (is_imx7ulp_lpuart(sport) && lpuart_uport_is_active(sport) &&
	    console_suspend_enabled && uart_console(uport)) {

		mutex_lock(&port->mutex);
		memset(&termios, 0, sizeof(struct ktermios));
		termios.c_cflag = uport->cons->cflag;
		if (port->tty && termios.c_cflag == 0)
			termios = port->tty->termios;
		uport->ops->set_termios(uport, &termios, NULL);
		mutex_unlock(&port->mutex);
	}
}

static int lpuart_resume(struct device *dev)
{
	struct lpuart_port *sport = dev_get_drvdata(dev);
	int ret;

	if (lpuart_uport_is_active(sport)) {
		if (lpuart_is_32(sport))
			lpuart32_hw_setup(sport);
		else
			lpuart_hw_setup(sport);
	} else if (pm_runtime_active(sport->port.dev)) {
		ret = lpuart_enable_clks(sport);
		if (ret)
			return ret;
		pm_runtime_set_active(sport->port.dev);
		pm_runtime_enable(sport->port.dev);
	}

	lpuart_console_fixup(sport);
	uart_resume_port(&lpuart_reg, &sport->port);

	return 0;
}

static const struct dev_pm_ops lpuart_pm_ops = {
	RUNTIME_PM_OPS(lpuart_runtime_suspend,
			   lpuart_runtime_resume, NULL)
	NOIRQ_SYSTEM_SLEEP_PM_OPS(lpuart_suspend_noirq,
				      lpuart_resume_noirq)
	SYSTEM_SLEEP_PM_OPS(lpuart_suspend, lpuart_resume)
};

static struct platform_driver lpuart_driver = {
	.probe		= lpuart_probe,
	.remove		= lpuart_remove,
	.driver		= {
		.name	= "fsl-lpuart",
		.of_match_table = lpuart_dt_ids,
		.pm	= pm_ptr(&lpuart_pm_ops),
	},
};

static int __init lpuart_serial_init(void)
{
	int ret = uart_register_driver(&lpuart_reg);

	if (ret)
		return ret;

	ret = platform_driver_register(&lpuart_driver);
	if (ret)
		uart_unregister_driver(&lpuart_reg);

	return ret;
}

static void __exit lpuart_serial_exit(void)
{
	platform_driver_unregister(&lpuart_driver);
	uart_unregister_driver(&lpuart_reg);
}

module_init(lpuart_serial_init);
module_exit(lpuart_serial_exit);

MODULE_DESCRIPTION("Freescale lpuart serial port driver");
MODULE_LICENSE("GPL v2");
