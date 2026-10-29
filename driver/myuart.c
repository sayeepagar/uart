// SPDX-License-Identifier: GPL-2.0
/*
 * myuart - interrupt-driven character driver for a 16550A-compatible UART
 *
 * Exposes one UART as /dev/myuart0:
 *   - read()/write() with blocking and O_NONBLOCK semantics
 *   - poll()/select()/epoll support
 *   - ioctl(): line settings, internal loopback, flush, statistics
 *   - /proc/myuart: live statistics
 *
 * Data path. Each FIFO has exactly one producer and one consumer, so the
 * kfifos need no lock of their own:
 *
 *   RX:  wire -> ISR -> rx_fifo -> read()
 *   TX:  write() -> tx_fifo -> ISR -> wire
 *
 * Locking:
 *   d->lock        spinlock (IRQ-safe). Guards every register access, the
 *                  IER/MCR shadows, the line config and the statistics.
 *                  The ISR takes it too.
 *   d->read_lock   mutex. Serialises readers (rx_fifo consumers).
 *   d->write_lock  mutex. Serialises writers (tx_fifo producers).
 *
 * Defaults match QEMU's second serial port (COM2: 0x2f8, IRQ 3). The first
 * one stays with the kernel's 8250 driver as the console (boot the guest
 * with 8250.nr_uarts=1 so the 8250 driver leaves COM2 alone).
 *
 * Port I/O (inb/outb) is wrapped in myuart_in()/myuart_out() so that a
 * memory-mapped variant only has to change those two helpers.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/serial_reg.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "../include/myuart_ioctl.h"

#define DRV_NAME	"myuart"
#define DEV_NAME	"myuart0"
#define FIFO_SIZE	4096		/* bytes per direction, power of two */
#define UART_BASE_BAUD	115200		/* 1.8432 MHz clock / 16 */
#define MAX_ISR_LOOP	256		/* bound the work done per interrupt */

/* IIR decoding: bit 0 = "nothing pending", bits 3..1 = interrupt source */
#define MYUART_IIR_NOINT	0x01
#define MYUART_IIR_ID_MASK	0x0e
#define MYUART_IIR_MSI		0x00
#define MYUART_IIR_THRI		0x02
#define MYUART_IIR_RDI		0x04
#define MYUART_IIR_RLSI		0x06
#define MYUART_IIR_TIMEOUT	0x0c
#define MYUART_IIR_FIFO_ON	0xc0	/* both bits set => 16550A FIFOs work */

static unsigned long io_base = 0x2f8;
module_param(io_base, ulong, 0444);
MODULE_PARM_DESC(io_base, "I/O port base of the UART (default 0x2f8 = COM2)");

static int irq_line = 3;
module_param_named(irq, irq_line, int, 0444);
MODULE_PARM_DESC(irq, "IRQ line of the UART (default 3 = COM2)");

static unsigned int init_baud = 115200;
module_param_named(baud, init_baud, uint, 0444);
MODULE_PARM_DESC(baud, "initial baud rate (default 115200)");

struct myuart_dev {
	unsigned long		base;
	int			irq;
	bool			has_fifo;
	unsigned int		tx_burst;	/* bytes per THRE interrupt */

	spinlock_t		lock;
	u8			fcr;		/* FIFO setup (write-only reg) */
	u8			ier;		/* shadow of IER */
	u8			mcr;		/* shadow of MCR */
	struct myuart_cfg	cfg;
	struct myuart_stats	stats;

	DECLARE_KFIFO(rx_fifo, u8, FIFO_SIZE);
	DECLARE_KFIFO(tx_fifo, u8, FIFO_SIZE);
	wait_queue_head_t	rx_wq;
	wait_queue_head_t	tx_wq;
	struct mutex		read_lock;
	struct mutex		write_lock;

	dev_t			devt;
	struct cdev		cdev;
	struct class		*cls;
	struct proc_dir_entry	*proc;
};

static struct myuart_dev *g_dev;	/* single instance */

/* ------------------------------------------------------------------ */
/* register access                                                     */
/* ------------------------------------------------------------------ */

static inline u8 myuart_in(struct myuart_dev *d, int reg)
{
	return inb(d->base + reg);
}

static inline void myuart_out(struct myuart_dev *d, int reg, u8 val)
{
	outb(val, d->base + reg);
}

/* ------------------------------------------------------------------ */
/* interrupt path                                                      */
/* ------------------------------------------------------------------ */

/* Drain the UART receiver into rx_fifo. Called with d->lock held. */
static unsigned int myuart_rx_chars(struct myuart_dev *d)
{
	unsigned int got = 0;
	int budget = MAX_ISR_LOOP;
	u8 lsr, ch;

	lockdep_assert_held(&d->lock);

	while (budget--) {
		lsr = myuart_in(d, UART_LSR);	/* reading LSR clears error bits */

		if (lsr & UART_LSR_OE)
			d->stats.hw_overruns++;
		if (!(lsr & UART_LSR_DR))
			break;

		ch = myuart_in(d, UART_RX);

		if (lsr & UART_LSR_BI) {		/* break: not data */
			d->stats.breaks++;
			continue;
		}
		if (lsr & UART_LSR_PE)
			d->stats.parity_errors++;
		if (lsr & UART_LSR_FE)
			d->stats.framing_errors++;
		if (lsr & (UART_LSR_PE | UART_LSR_FE))	/* drop damaged bytes */
			continue;

		d->stats.rx_bytes++;
		if (!kfifo_put(&d->rx_fifo, ch))
			d->stats.sw_overruns++;
		else
			got++;
	}
	return got;
}

/* Feed the UART transmitter from tx_fifo. Called with d->lock held. */
static void myuart_tx_chars(struct myuart_dev *d)
{
	unsigned int n = d->tx_burst;
	u8 ch;

	lockdep_assert_held(&d->lock);

	while (n-- && kfifo_get(&d->tx_fifo, &ch)) {
		myuart_out(d, UART_TX, ch);
		d->stats.tx_bytes++;
	}

	/*
	 * Nothing left: stop THRE interrupts, otherwise they would fire
	 * forever. This check and write() -> myuart_kick_tx() both run under
	 * d->lock, and kick_tx() runs *after* the data is in the fifo, so a
	 * byte queued concurrently can never be stranded.
	 */
	if (kfifo_is_empty(&d->tx_fifo)) {
		d->ier &= ~UART_IER_THRI;
		myuart_out(d, UART_IER, d->ier);
	}
}

static irqreturn_t myuart_isr(int irq, void *dev_id)
{
	struct myuart_dev *d = dev_id;
	bool handled = false, rx = false, tx = false;
	int budget = MAX_ISR_LOOP;
	u8 iir;

	spin_lock(&d->lock);
	while (budget--) {
		iir = myuart_in(d, UART_IIR);
		if (iir & MYUART_IIR_NOINT)
			break;
		handled = true;

		switch (iir & MYUART_IIR_ID_MASK) {
		case MYUART_IIR_RLSI:
		case MYUART_IIR_RDI:
		case MYUART_IIR_TIMEOUT:
			if (myuart_rx_chars(d))
				rx = true;
			break;
		case MYUART_IIR_THRI:
			myuart_tx_chars(d);
			tx = true;
			break;
		case MYUART_IIR_MSI:
		default:
			(void)myuart_in(d, UART_MSR);	/* reading MSR clears it */
			break;
		}
	}
	if (handled)
		d->stats.irq_count++;
	spin_unlock(&d->lock);

	if (rx)
		wake_up_interruptible(&d->rx_wq);
	if (tx)
		wake_up_interruptible(&d->tx_wq);

	return IRQ_RETVAL(handled);
}

/* Enable the "transmitter empty" interrupt; the ISR does the sending. */
static void myuart_kick_tx(struct myuart_dev *d)
{
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	if (!(d->ier & UART_IER_THRI)) {
		d->ier |= UART_IER_THRI;
		myuart_out(d, UART_IER, d->ier);
	}
	spin_unlock_irqrestore(&d->lock, flags);
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static int myuart_apply_cfg(struct myuart_dev *d, const struct myuart_cfg *c)
{
	unsigned long flags;
	unsigned int div;
	u8 lcr;

	if (c->baud < 300 || c->baud > UART_BASE_BAUD)
		return -EINVAL;
	if (c->data_bits < 5 || c->data_bits > 8)
		return -EINVAL;
	if (c->parity > MYUART_PARITY_EVEN)
		return -EINVAL;
	if (c->stop_bits < 1 || c->stop_bits > 2)
		return -EINVAL;

	div = DIV_ROUND_CLOSEST(UART_BASE_BAUD, c->baud);
	if (!div || div > 0xffff)
		return -EINVAL;

	lcr = c->data_bits - 5;			/* WLEN5..WLEN8 = 0..3 */
	if (c->stop_bits == 2)
		lcr |= UART_LCR_STOP;
	if (c->parity != MYUART_PARITY_NONE) {
		lcr |= UART_LCR_PARITY;
		if (c->parity == MYUART_PARITY_EVEN)
			lcr |= UART_LCR_EPAR;
	}

	/*
	 * While DLAB is set, offset 0/1 are the divisor latches instead of
	 * RBR/THR/IER. The ISR must not run in that window, hence the lock.
	 */
	spin_lock_irqsave(&d->lock, flags);
	myuart_out(d, UART_LCR, lcr | UART_LCR_DLAB);
	myuart_out(d, UART_DLL, div & 0xff);
	myuart_out(d, UART_DLM, div >> 8);
	myuart_out(d, UART_LCR, lcr);

	d->cfg = *c;
	d->cfg.baud = UART_BASE_BAUD / div;	/* what we really got */
	d->cfg.reserved = 0;
	spin_unlock_irqrestore(&d->lock, flags);
	return 0;
}

static void myuart_set_loopback(struct myuart_dev *d, bool on)
{
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	if (on)
		d->mcr |= UART_MCR_LOOP;
	else
		d->mcr &= ~UART_MCR_LOOP;
	myuart_out(d, UART_MCR, d->mcr);
	spin_unlock_irqrestore(&d->lock, flags);
}

static int myuart_flush(struct myuart_dev *d)
{
	unsigned long flags;

	/* read_lock keeps readers out while we act as the rx_fifo consumer */
	if (mutex_lock_interruptible(&d->read_lock))
		return -ERESTARTSYS;

	spin_lock_irqsave(&d->lock, flags);
	kfifo_reset_out(&d->rx_fifo);
	kfifo_reset_out(&d->tx_fifo);	/* lock held: we stand in for the ISR */
	myuart_out(d, UART_FCR,
		   d->fcr | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
	spin_unlock_irqrestore(&d->lock, flags);

	mutex_unlock(&d->read_lock);
	wake_up_interruptible(&d->tx_wq);
	return 0;
}

static void myuart_snapshot(struct myuart_dev *d, struct myuart_stats *s,
			    struct myuart_cfg *c)
{
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	*s = d->stats;
	if (c)
		*c = d->cfg;
	s->rx_fifo_level = kfifo_len(&d->rx_fifo);
	s->tx_fifo_level = kfifo_len(&d->tx_fifo);
	s->loopback = !!(d->mcr & UART_MCR_LOOP);
	spin_unlock_irqrestore(&d->lock, flags);
}

/* Probe the chip and bring it to a known, quiet state (IRQs off). */
static int myuart_hw_init(struct myuart_dev *d)
{
	u8 iir;

	/* Scratch register test: an absent port floats high and reads 0xff */
	myuart_out(d, UART_SCR, 0xa5);
	if (myuart_in(d, UART_SCR) != 0xa5)
		return -ENODEV;
	myuart_out(d, UART_SCR, 0x5a);
	if (myuart_in(d, UART_SCR) != 0x5a)
		return -ENODEV;

	myuart_out(d, UART_IER, 0);

	d->fcr = UART_FCR_ENABLE_FIFO | UART_FCR_R_TRIG_00;	/* 1-byte trigger */
	myuart_out(d, UART_FCR,
		   d->fcr | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
	iir = myuart_in(d, UART_IIR);
	d->has_fifo = (iir & MYUART_IIR_FIFO_ON) == MYUART_IIR_FIFO_ON;
	d->tx_burst = d->has_fifo ? 16 : 1;

	/* clear anything pending */
	(void)myuart_in(d, UART_LSR);
	(void)myuart_in(d, UART_RX);
	(void)myuart_in(d, UART_IIR);
	(void)myuart_in(d, UART_MSR);

	/* DTR+RTS, and OUT2 which gates the IRQ line on a PC UART */
	d->mcr = UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2;
	myuart_out(d, UART_MCR, d->mcr);
	d->ier = 0;
	return 0;
}

static void myuart_hw_quiesce(struct myuart_dev *d)
{
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	d->ier = 0;
	d->mcr = 0;
	myuart_out(d, UART_IER, 0);
	myuart_out(d, UART_MCR, 0);
	spin_unlock_irqrestore(&d->lock, flags);
}

/* ------------------------------------------------------------------ */
/* file operations                                                     */
/* ------------------------------------------------------------------ */

static int myuart_open(struct inode *inode, struct file *f)
{
	f->private_data = container_of(inode->i_cdev, struct myuart_dev, cdev);
	return nonseekable_open(inode, f);
}

static int myuart_release(struct inode *inode, struct file *f)
{
	return 0;
}

static ssize_t myuart_read(struct file *f, char __user *buf, size_t count,
			   loff_t *ppos)
{
	struct myuart_dev *d = f->private_data;
	unsigned int copied = 0;
	int ret;

	if (!count)
		return 0;

	if (mutex_lock_interruptible(&d->read_lock))
		return -ERESTARTSYS;

	while (kfifo_is_empty(&d->rx_fifo)) {
		mutex_unlock(&d->read_lock);

		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(d->rx_wq,
					     !kfifo_is_empty(&d->rx_fifo)))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&d->read_lock))
			return -ERESTARTSYS;
	}

	/* copy_to_user may fault and sleep: no spinlock here, none needed */
	ret = kfifo_to_user(&d->rx_fifo, buf, count, &copied);
	mutex_unlock(&d->read_lock);

	return ret ? ret : (ssize_t)copied;
}

static ssize_t myuart_write(struct file *f, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct myuart_dev *d = f->private_data;
	size_t done = 0;
	int ret = 0;

	if (!count)
		return 0;

	if (mutex_lock_interruptible(&d->write_lock))
		return -ERESTARTSYS;

	while (done < count) {
		unsigned int n = 0;

		if (kfifo_is_full(&d->tx_fifo)) {
			if (f->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}
			if (wait_event_interruptible(d->tx_wq,
						!kfifo_is_full(&d->tx_fifo))) {
				ret = -ERESTARTSYS;
				break;
			}
			continue;
		}

		ret = kfifo_from_user(&d->tx_fifo, buf + done, count - done, &n);
		if (ret)
			break;
		done += n;
		myuart_kick_tx(d);	/* only after the bytes are queued */
	}

	mutex_unlock(&d->write_lock);
	return done ? (ssize_t)done : ret;	/* partial write beats an error */
}

static __poll_t myuart_poll(struct file *f, poll_table *wait)
{
	struct myuart_dev *d = f->private_data;
	__poll_t mask = 0;

	poll_wait(f, &d->rx_wq, wait);
	poll_wait(f, &d->tx_wq, wait);

	if (!kfifo_is_empty(&d->rx_fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!kfifo_is_full(&d->tx_fifo))
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static long myuart_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct myuart_dev *d = f->private_data;
	void __user *uarg = (void __user *)arg;
	struct myuart_cfg cfg;
	struct myuart_stats st;
	unsigned long flags;
	int on;

	switch (cmd) {
	case MYUART_IOC_SET_CFG:
		if (copy_from_user(&cfg, uarg, sizeof(cfg)))
			return -EFAULT;
		return myuart_apply_cfg(d, &cfg);

	case MYUART_IOC_GET_CFG:
		spin_lock_irqsave(&d->lock, flags);
		cfg = d->cfg;
		spin_unlock_irqrestore(&d->lock, flags);
		return copy_to_user(uarg, &cfg, sizeof(cfg)) ? -EFAULT : 0;

	case MYUART_IOC_GET_STATS:
		myuart_snapshot(d, &st, NULL);
		return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;

	case MYUART_IOC_CLR_STATS:
		spin_lock_irqsave(&d->lock, flags);
		memset(&d->stats, 0, sizeof(d->stats));
		spin_unlock_irqrestore(&d->lock, flags);
		return 0;

	case MYUART_IOC_FLUSH:
		return myuart_flush(d);

	case MYUART_IOC_LOOPBACK:
		if (get_user(on, (int __user *)uarg))
			return -EFAULT;
		myuart_set_loopback(d, !!on);
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations myuart_fops = {
	.owner		= THIS_MODULE,
	.open		= myuart_open,
	.release	= myuart_release,
	.read		= myuart_read,
	.write		= myuart_write,
	.poll		= myuart_poll,
	.unlocked_ioctl	= myuart_ioctl,
};

/* ------------------------------------------------------------------ */
/* /proc/myuart                                                        */
/* ------------------------------------------------------------------ */

static int myuart_proc_show(struct seq_file *m, void *v)
{
	struct myuart_dev *d = m->private;
	struct myuart_stats s;
	struct myuart_cfg c;
	static const char parity_ch[] = { 'N', 'O', 'E' };

	myuart_snapshot(d, &s, &c);

	seq_printf(m, "port:           0x%lx irq %d, %s\n", d->base, d->irq,
		   d->has_fifo ? "16550A (16-byte FIFOs)" : "8250/16450 (no FIFO)");
	seq_printf(m, "line:           %u %u%c%u%s\n", c.baud, c.data_bits,
		   parity_ch[c.parity % 3], c.stop_bits,
		   s.loopback ? "  [internal loopback ON]" : "");
	seq_printf(m, "rx_bytes:       %llu\n", (unsigned long long)s.rx_bytes);
	seq_printf(m, "tx_bytes:       %llu\n", (unsigned long long)s.tx_bytes);
	seq_printf(m, "irq_count:      %u\n", s.irq_count);
	seq_printf(m, "rx_fifo:        %u / %d\n", s.rx_fifo_level, FIFO_SIZE);
	seq_printf(m, "tx_fifo:        %u / %d\n", s.tx_fifo_level, FIFO_SIZE);
	seq_printf(m, "sw_overruns:    %u\n", s.sw_overruns);
	seq_printf(m, "hw_overruns:    %u\n", s.hw_overruns);
	seq_printf(m, "parity_errors:  %u\n", s.parity_errors);
	seq_printf(m, "framing_errors: %u\n", s.framing_errors);
	seq_printf(m, "breaks:         %u\n", s.breaks);
	return 0;
}

/* ------------------------------------------------------------------ */
/* module init / exit                                                  */
/* ------------------------------------------------------------------ */

static int __init myuart_init(void)
{
	struct myuart_cfg cfg = {
		.baud		= init_baud,
		.data_bits	= 8,
		.parity		= MYUART_PARITY_NONE,
		.stop_bits	= 1,
	};
	struct myuart_dev *d;
	struct device *dev;
	unsigned long flags;
	int ret;

	if (!request_region(io_base, 8, DRV_NAME)) {
		pr_err("I/O ports 0x%lx-0x%lx are busy. Is the 8250 driver using them? Boot with 8250.nr_uarts=1\n",
		       io_base, io_base + 7);
		return -EBUSY;
	}

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d) {
		ret = -ENOMEM;
		goto err_region;
	}
	d->base = io_base;
	d->irq = irq_line;
	spin_lock_init(&d->lock);
	mutex_init(&d->read_lock);
	mutex_init(&d->write_lock);
	init_waitqueue_head(&d->rx_wq);
	init_waitqueue_head(&d->tx_wq);
	INIT_KFIFO(d->rx_fifo);
	INIT_KFIFO(d->tx_fifo);

	ret = myuart_hw_init(d);
	if (ret) {
		pr_err("no 16550 UART found at 0x%lx (QEMU needs a second -serial option)\n",
		       io_base);
		goto err_free;
	}

	ret = myuart_apply_cfg(d, &cfg);
	if (ret) {
		pr_err("invalid baud rate %u\n", init_baud);
		goto err_hw;
	}

	/* IER is still 0, so the UART cannot raise anything yet */
	ret = request_irq(d->irq, myuart_isr, 0, DRV_NAME, d);
	if (ret) {
		pr_err("cannot get IRQ %d (%d)\n", d->irq, ret);
		goto err_hw;
	}

	ret = alloc_chrdev_region(&d->devt, 0, 1, DRV_NAME);
	if (ret)
		goto err_irq;

	cdev_init(&d->cdev, &myuart_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, d->devt, 1);
	if (ret)
		goto err_chrdev;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	d->cls = class_create(DRV_NAME);
#else
	d->cls = class_create(THIS_MODULE, DRV_NAME);
#endif
	if (IS_ERR(d->cls)) {
		ret = PTR_ERR(d->cls);
		goto err_cdev;
	}

	dev = device_create(d->cls, NULL, d->devt, d, DEV_NAME);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_class;
	}

	/* everything is ready: let the UART talk */
	spin_lock_irqsave(&d->lock, flags);
	d->ier = UART_IER_RDI | UART_IER_RLSI;
	myuart_out(d, UART_IER, d->ier);
	spin_unlock_irqrestore(&d->lock, flags);

	d->proc = proc_create_single_data(DRV_NAME, 0444, NULL,
					  myuart_proc_show, d);
	if (!d->proc)
		pr_warn("could not create /proc/%s\n", DRV_NAME);

	g_dev = d;
	pr_info("loaded: /dev/%s (%d:%d), port 0x%lx irq %d, %u baud, %s\n",
		DEV_NAME, MAJOR(d->devt), MINOR(d->devt), d->base, d->irq,
		d->cfg.baud, d->has_fifo ? "16550A FIFO" : "no FIFO");
	return 0;

err_class:
	class_destroy(d->cls);
err_cdev:
	cdev_del(&d->cdev);
err_chrdev:
	unregister_chrdev_region(d->devt, 1);
err_irq:
	free_irq(d->irq, d);
err_hw:
	myuart_hw_quiesce(d);
err_free:
	kfree(d);
err_region:
	release_region(io_base, 8);
	return ret;
}

static void __exit myuart_exit(void)
{
	struct myuart_dev *d = g_dev;

	if (d->proc)
		proc_remove(d->proc);
	device_destroy(d->cls, d->devt);
	class_destroy(d->cls);
	cdev_del(&d->cdev);
	unregister_chrdev_region(d->devt, 1);

	myuart_hw_quiesce(d);		/* silence the chip, then drop the IRQ */
	free_irq(d->irq, d);
	release_region(d->base, 8);

	pr_info("unloaded (rx %llu bytes, tx %llu bytes)\n",
		(unsigned long long)d->stats.rx_bytes,
		(unsigned long long)d->stats.tx_bytes);
	kfree(d);
}

module_init(myuart_init);
module_exit(myuart_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sayee");
MODULE_DESCRIPTION("Interrupt-driven character driver for a 16550A UART");
