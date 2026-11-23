// SPDX-License-Identifier: GPL-2.0-only
/*
 * Early console for the HiSilicon UART V500.
 *
 * Minimal port of the vendor earlycon from:
 *   platform_source/basicplatform/drivers/tty/serial/uart_v500_console.c
 *
 * The bootloader/UEFI is expected to have already configured the UART.
 * This driver only polls the TX FIFO and writes characters.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>

#define UART_V500_TX_FIFO	0x00c
#define UART_V500_STAT		0x014
#define UART_V500_STAT_TX_BUSY	0x001
#define UART_V500_STAT_TXFF	0x008

static void uart_v500_early_putc(struct uart_port *port, unsigned char c)
{
	unsigned int timeout = 100000;

	while ((readl(port->membase + UART_V500_STAT) & UART_V500_STAT_TXFF) &&
	       --timeout)
		cpu_relax();

	/* UART may not be enabled by the firmware; never hang earlycon. */
	if (!timeout)
		return;

	writel(c, port->membase + UART_V500_TX_FIFO);

	timeout = 100000;
	while ((readl(port->membase + UART_V500_STAT) & UART_V500_STAT_TX_BUSY) &&
	       --timeout)
		cpu_relax();
}

static void uart_v500_early_write(struct console *con, const char *s,
				  unsigned int n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, uart_v500_early_putc);
}

static int __init uart_v500_early_setup(struct earlycon_device *device,
					const char *options)
{
	if (!device->port.membase)
		return -ENODEV;

	device->port.iotype = UPIO_MEM32;
	device->con->write = uart_v500_early_write;
	return 0;
}

OF_EARLYCON_DECLARE(uart_v500, "hisilicon,uart", uart_v500_early_setup);
