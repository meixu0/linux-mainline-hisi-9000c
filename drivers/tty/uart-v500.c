// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal serial driver for the HiSilicon UART V500.
 *
 * Register layout is taken from the vendor driver (GPL-2.0),
 * platform_source/basicplatform/drivers/tty/serial/uart_v500.c.
 *
 * This port is deliberately PIO-only: no dmaengine, pinctrl, reset or
 * clock-framework dependencies.  The firmware already leaves the console
 * UART enabled at 115200n8, and mainline 6.18 has no clock provider for
 * this SoC yet, so:
 *
 *   - startup() only unmasks the RX/TX interrupts and clears state;
 *   - set_termios() reprograms baud/frame only when userspace asks for
 *     something other than the firmware default (115200 8N1).
 *
 * That keeps the known-good firmware configuration intact while giving a
 * real /dev/ttyHW0 console (input and output).
 */

#include <linux/console.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define V500_NR_PORTS		8
#define V500_FIFO_SIZE		64
#define V500_DEFAULT_UARTCLK	112000000	/* 112 MHz, vendor DT clock-rate */

/* Register offsets */
#define V500_BAUD_CFG		0x000
#define V500_FRAME_CFG		0x004
#define V500_FIFO_CFG		0x008
#define V500_TX_FIFO		0x00c
#define V500_RX_FIFO		0x010
#define V500_STAT		0x014
#define V500_INT_CLR		0x018
#define V500_INT_MSK		0x01c
#define V500_INT_RAW		0x020
#define V500_INT_STAT		0x024
#define V500_CONFIG		0x028

/* STAT */
#define V500_STAT_TXFE		(1 << 2)
#define V500_STAT_TXFF		(1 << 3)
#define V500_STAT_RXFE		(1 << 4)

/* Interrupt bits (same bit positions in MSK/CLR/RAW/STATUS) */
#define V500_INT_RXS		(1 << 0)	/* rx data available */
#define V500_INT_TXS		(1 << 1)	/* tx fifo level */
#define V500_INT_RTS		(1 << 2)	/* rx timeout */
#define V500_INT_ALL		0x7ff

/* FIFO_CFG: TX trigger [2:0], RX trigger [5:3], RTS trigger [8:6] */
#define V500_FIFO_TX_1_2	(2 << 0)
#define V500_FIFO_RX_1_8	(0 << 3)
#define V500_FIFO_RTS_7_8	(4 << 6)

/* Per-character error bits carried in the RX FIFO word */
#define V500_DR_FE		(1 << 8)
#define V500_DR_PE		(1 << 9)
#define V500_DR_BE		(1 << 10)
#define V500_DR_OE		(1 << 11)
#define V500_DR_ERR		(V500_DR_FE | V500_DR_PE | V500_DR_BE | V500_DR_OE)
#define V500_RX_DATA		0xff

/* CONFIG bits (vendor values; low half = set, high half = clear) */
#define V500_CFG_UART_EN	0x00010001
#define V500_CFG_TX_EN		0x00020002
#define V500_CFG_RX_EN		0x00040004
#define V500_CFG_FIFO_EN	0x04000400
#define V500_CFG_DIS_ALL	0xffff0000

/* FRAME_CFG */
#define V500_FRAME_STOP		(1 << 5)
#define V500_FRAME_PEN		(1 << 4)

#define V500_BAUD_FRAC_MASK	0x3f
#define V500_BAUD_INT_MASK	0xffff
#define V500_CONFIG_BIT_MASK	16
#define V500_ISR_PASS_LIMIT	256

struct v500_port {
	struct uart_port	port;
};

#define to_v500_port(p)	container_of(p, struct v500_port, port)

static struct uart_driver v500_uart_driver;

#ifdef CONFIG_SERIAL_UART_V500_CONSOLE
static struct console v500_console;
#endif

static DEFINE_XARRAY_FLAGS(v500_ports, XA_FLAGS_ALLOC);

static u32 v500_read(struct uart_port *port, u32 off)
{
	return readl(port->membase + off);
}

static void v500_write(struct uart_port *port, u32 val, u32 off)
{
	writel(val, port->membase + off);
}

static unsigned int v500_tx_empty(struct uart_port *port)
{
	return (v500_read(port, V500_STAT) & V500_STAT_TXFE) ? TIOCSER_TEMT : 0;
}

static void v500_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int v500_get_mctrl(struct uart_port *port)
{
	return TIOCM_DSR | TIOCM_CTS | TIOCM_CAR;
}

static void v500_stop_tx(struct uart_port *port)
{
	v500_write(port, v500_read(port, V500_INT_MSK) | V500_INT_TXS,
		   V500_INT_MSK);
}

static void v500_tx_chars(struct uart_port *port)
{
	u8 ch;

	uart_port_tx_limited(port, ch, port->fifosize >> 1,
			     !(v500_read(port, V500_STAT) & V500_STAT_TXFF),
			     v500_write(port, ch, V500_TX_FIFO),
			     ({}));
}

static void v500_start_tx(struct uart_port *port)
{
	v500_write(port, v500_read(port, V500_INT_MSK) & ~V500_INT_TXS,
		   V500_INT_MSK);
	v500_tx_chars(port);
}

static void v500_stop_rx(struct uart_port *port)
{
	v500_write(port, v500_read(port, V500_INT_MSK) | V500_INT_RXS,
		   V500_INT_MSK);
}

/* must be called with the port lock held; caller does the flip push */
static void v500_rx_chars(struct uart_port *port)
{
	unsigned int max_count = V500_FIFO_SIZE * 4;

	while (max_count--) {
		u32 ch, flag = TTY_NORMAL;

		if (v500_read(port, V500_STAT) & V500_STAT_RXFE)
			break;

		ch = v500_read(port, V500_RX_FIFO);
		port->icount.rx++;

		if (unlikely(ch & V500_DR_ERR)) {
			if (ch & V500_DR_BE) {
				port->icount.brk++;
				flag = TTY_BREAK;
			} else if (ch & V500_DR_PE) {
				port->icount.parity++;
				flag = TTY_PARITY;
			} else if (ch & V500_DR_FE) {
				port->icount.frame++;
				flag = TTY_FRAME;
			}
			if (ch & V500_DR_OE)
				port->icount.overrun++;
			ch &= V500_RX_DATA;
			uart_insert_char(port, ch, 0, ch, flag);
			continue;
		}

		ch &= V500_RX_DATA;
		if (uart_handle_sysrq_char(port, ch))
			continue;
		uart_insert_char(port, ch, 0, ch, TTY_NORMAL);
	}
}

static irqreturn_t v500_int_handler(int irq, void *dev_id)
{
	struct uart_port *port = dev_id;
	unsigned long flags;
	unsigned int status, pass = V500_ISR_PASS_LIMIT;
	bool handled = false, push = false;

	uart_port_lock_irqsave(port, &flags);

	status = v500_read(port, V500_INT_STAT);
	while (status && pass--) {
		u32 clr = status & ~(V500_INT_RXS | V500_INT_TXS);

		if (clr)
			v500_write(port, clr, V500_INT_CLR);
		if (status & (V500_INT_RXS | V500_INT_RTS)) {
			v500_rx_chars(port);
			push = true;
		}
		if (status & V500_INT_TXS)
			v500_tx_chars(port);

		handled = true;
		status = v500_read(port, V500_INT_STAT);
	}

	uart_port_unlock_irqrestore(port, flags);

	if (push)
		tty_flip_buffer_push(&port->state->port);

	return IRQ_RETVAL(handled);
}

static void v500_enable_ms(struct uart_port *port)
{
}

static int v500_startup(struct uart_port *port)
{
	u32 mask;
	int ret;

	/*
	 * The firmware leaves the RX trigger at 1/2 (32 bytes), which makes
	 * the console unusable interactively: nothing is delivered until 32
	 * characters have accumulated.  Use 1/8 and rely on the rx-timeout
	 * interrupt to flush partial bursts (single keystrokes).
	 */
	v500_write(port, V500_FIFO_TX_1_2 | V500_FIFO_RX_1_8 |
		   V500_FIFO_RTS_7_8, V500_FIFO_CFG);

	/* Clear stale error / rx / tx state left by the firmware. */
	v500_write(port, V500_INT_ALL, V500_INT_CLR);

	ret = request_irq(port->irq, v500_int_handler, 0, "uart-v500", port);
	if (ret)
		return ret;

	/* Mask everything but RX data, rx-timeout and TX-empty. */
	mask = V500_INT_ALL & ~(V500_INT_RXS | V500_INT_TXS | V500_INT_RTS);
	v500_write(port, mask, V500_INT_MSK);

	return 0;
}

static void v500_shutdown(struct uart_port *port)
{
	v500_write(port, V500_INT_ALL, V500_INT_MSK);
	v500_write(port, V500_INT_ALL, V500_INT_CLR);
	free_irq(port->irq, port);
}

static void v500_set_termios(struct uart_port *port, struct ktermios *termios,
			     const struct ktermios *old)
{
	unsigned int baud, quot, frame = 0;

	baud = uart_get_baud_rate(port, termios, old, 0, port->uartclk / 16);
	if (!baud)
		return;

	uart_update_timeout(port, termios->c_cflag, baud);

	/*
	 * The firmware already configured the console UART for 115200 8N1;
	 * do not touch a known-good configuration for the common case.
	 */
	if (baud == 115200 && (termios->c_cflag & CSIZE) == CS8 &&
	    !(termios->c_cflag & (PARENB | CSTOPB)))
		return;

	if (baud > port->uartclk / 16)
		quot = DIV_ROUND_CLOSEST(port->uartclk * 8, baud);
	else
		quot = DIV_ROUND_CLOSEST(port->uartclk * 4, baud);

	switch (termios->c_cflag & CSIZE) {
	case CS5: frame = 0; break;
	case CS6: frame = 1; break;
	case CS7: frame = 2; break;
	default:  frame = 3; break;
	}
	if (termios->c_cflag & CSTOPB)
		frame |= V500_FRAME_STOP;
	if (termios->c_cflag & PARENB)
		frame |= V500_FRAME_PEN;
	v500_write(port, frame, V500_FRAME_CFG);

	/* Re-enable the block, then program the divisor. */
	v500_write(port, V500_CFG_DIS_ALL, V500_CONFIG);
	v500_write(port, V500_CFG_UART_EN | V500_CFG_TX_EN |
		   V500_CFG_RX_EN | V500_CFG_FIFO_EN, V500_CONFIG);

	v500_write(port, ((quot & V500_BAUD_FRAC_MASK) << V500_CONFIG_BIT_MASK) |
		   ((quot >> 6) & V500_BAUD_INT_MASK), V500_BAUD_CFG);
}

static const char *v500_type(struct uart_port *port)
{
	return "UART-V500";
}

static void v500_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_16550A;
}

static int v500_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != port->type)
		return -EINVAL;
	return 0;
}

static const struct uart_ops v500_ops = {
	.tx_empty	= v500_tx_empty,
	.set_mctrl	= v500_set_mctrl,
	.get_mctrl	= v500_get_mctrl,
	.stop_tx	= v500_stop_tx,
	.start_tx	= v500_start_tx,
	.stop_rx	= v500_stop_rx,
	.enable_ms	= v500_enable_ms,
	.startup	= v500_startup,
	.shutdown	= v500_shutdown,
	.set_termios	= v500_set_termios,
	.type		= v500_type,
	.config_port	= v500_config_port,
	.verify_port	= v500_verify_port,
};

static int v500_probe(struct platform_device *pdev)
{
	struct v500_port *up;
	struct uart_port *port;
	struct resource *res;
	struct xa_limit limit;
	u32 clk = V500_DEFAULT_UARTCLK;
	int irq, line, ret;

	up = devm_kzalloc(&pdev->dev, sizeof(*up), GFP_KERNEL);
	if (!up)
		return -ENOMEM;

	port = &up->port;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	port->membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(port->membase))
		return PTR_ERR(port->membase);
	port->mapbase = res->start;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	of_property_read_u32(pdev->dev.of_node, "clock-frequency", &clk);

	line = of_alias_get_id(pdev->dev.of_node, "serial");
	limit = (line < 0) ? XA_LIMIT(0, V500_NR_PORTS - 1) : XA_LIMIT(line, line);

	ret = xa_alloc(&v500_ports, &line, up, limit, GFP_KERNEL);
	if (ret)
		return ret;

	port->dev	= &pdev->dev;
	port->iotype	= UPIO_MEM32;
	port->irq	= irq;
	port->uartclk	= clk;
	port->fifosize	= V500_FIFO_SIZE;
	port->flags	= UPF_BOOT_AUTOCONF;
	port->ops	= &v500_ops;
	port->type	= PORT_UNKNOWN;
	port->line	= line;
	spin_lock_init(&port->lock);

	platform_set_drvdata(pdev, up);

	ret = uart_add_one_port(&v500_uart_driver, port);
	if (ret) {
		xa_erase(&v500_ports, line);
		return ret;
	}

	dev_info(&pdev->dev, "ttyHW%d irq %d uartclk %u\n", line, irq, clk);
	return 0;
}

static void v500_remove(struct platform_device *pdev)
{
	struct v500_port *up = platform_get_drvdata(pdev);
	int line = up->port.line;

	uart_remove_one_port(&v500_uart_driver, &up->port);
	xa_erase(&v500_ports, line);
}

static const struct of_device_id v500_of_match[] = {
	{ .compatible = "hisilicon,uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, v500_of_match);

static struct platform_driver v500_platform_driver = {
	.probe	= v500_probe,
	.remove	= v500_remove,
	.driver	= {
		.name		= "uart-v500",
		.of_match_table	= v500_of_match,
	},
};

static struct uart_driver v500_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "uart-v500",
	.dev_name	= "ttyHW",
	.nr		= V500_NR_PORTS,
#ifdef CONFIG_SERIAL_UART_V500_CONSOLE
	.cons		= &v500_console,
#endif
};

#ifdef CONFIG_SERIAL_UART_V500_CONSOLE

static void v500_putchar(struct uart_port *port, unsigned char ch)
{
	/*
	 * Block until the FIFO has room.  Dropping characters here makes
	 * console logs unusable under burst load (the FIFO can stay full for
	 * several milliseconds at 115200).
	 */
	while (v500_read(port, V500_STAT) & V500_STAT_TXFF)
		cpu_relax();
	v500_write(port, ch, V500_TX_FIFO);
}

static void v500_console_write(struct console *co, const char *s,
			       unsigned int count)
{
	struct v500_port *up;
	struct uart_port *port;
	unsigned long flags;

	up = xa_load(&v500_ports, co->index);
	if (!up)
		return;
	port = &up->port;

	uart_port_lock_irqsave(port, &flags);
	uart_console_write(port, s, count, v500_putchar);
	uart_port_unlock_irqrestore(port, flags);
}

static int v500_console_setup(struct console *co, char *options)
{
	struct v500_port *up;
	struct uart_port *port;
	int baud = 115200, bits = 8, parity = 'n', flow = 'n';

	up = xa_load(&v500_ports, co->index);
	if (!up)
		return -ENODEV;
	port = &up->port;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static struct console v500_console = {
	.name	= "ttyHW",
	.write	= v500_console_write,
	.device	= uart_console_device,
	.setup	= v500_console_setup,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
	.data	= &v500_uart_driver,
};

static int __init v500_console_init(void)
{
	register_console(&v500_console);
	return 0;
}
console_initcall(v500_console_init);

#endif /* CONFIG_SERIAL_UART_V500_CONSOLE */

static int __init v500_init(void)
{
	int ret;

	ret = uart_register_driver(&v500_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&v500_platform_driver);
	if (ret)
		uart_unregister_driver(&v500_uart_driver);

	return ret;
}
device_initcall(v500_init);

static void __exit v500_exit(void)
{
	platform_driver_unregister(&v500_platform_driver);
	uart_unregister_driver(&v500_uart_driver);
}
module_exit(v500_exit);

MODULE_AUTHOR("charlotte bring-up");
MODULE_DESCRIPTION("HiSilicon UART V500 serial driver (PIO)");
MODULE_LICENSE("GPL v2");
