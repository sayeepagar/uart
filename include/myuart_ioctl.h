/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * myuart_ioctl.h - user/kernel ABI of the myuart character driver.
 *
 * Shared by the kernel module (driver/myuart.c) and user space
 * (app/myuart_test.c), so it only uses the fixed-width __uNN types.
 */
#ifndef MYUART_IOCTL_H
#define MYUART_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MYUART_IOC_MAGIC	'u'

enum {
	MYUART_PARITY_NONE = 0,
	MYUART_PARITY_ODD  = 1,
	MYUART_PARITY_EVEN = 2,
};

/* Line configuration. Valid: baud 300..115200, 5..8 data bits, 1..2 stop. */
struct myuart_cfg {
	__u32 baud;		/* the driver reports the rate it really got */
	__u8  data_bits;
	__u8  parity;		/* MYUART_PARITY_* */
	__u8  stop_bits;
	__u8  reserved;
};

/* Snapshot of the driver counters. Sizes are fixed: 56 bytes. */
struct myuart_stats {
	__u64 rx_bytes;		/* bytes taken from the UART by the ISR */
	__u64 tx_bytes;		/* bytes written to the UART by the ISR */
	__u32 irq_count;
	__u32 sw_overruns;	/* rx_fifo full: byte dropped by the driver */
	__u32 hw_overruns;	/* UART RX FIFO overflowed before the ISR ran */
	__u32 parity_errors;
	__u32 framing_errors;
	__u32 breaks;
	__u32 rx_fifo_level;	/* bytes waiting for read() */
	__u32 tx_fifo_level;	/* bytes waiting for the ISR to send */
	__u32 loopback;		/* 1 if internal loopback is enabled */
	__u32 reserved;
};

#define MYUART_IOC_SET_CFG	_IOW(MYUART_IOC_MAGIC, 1, struct myuart_cfg)
#define MYUART_IOC_GET_CFG	_IOR(MYUART_IOC_MAGIC, 2, struct myuart_cfg)
#define MYUART_IOC_GET_STATS	_IOR(MYUART_IOC_MAGIC, 3, struct myuart_stats)
#define MYUART_IOC_CLR_STATS	_IO(MYUART_IOC_MAGIC, 4)
#define MYUART_IOC_FLUSH	_IO(MYUART_IOC_MAGIC, 5)	/* drop RX+TX data */
#define MYUART_IOC_LOOPBACK	_IOW(MYUART_IOC_MAGIC, 6, int)	/* 0 / 1 */

#endif /* MYUART_IOCTL_H */
