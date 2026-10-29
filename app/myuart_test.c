// SPDX-License-Identifier: GPL-2.0
/*
 * myuart_test - user-space exerciser for /dev/myuart0
 *
 *   myuart_test selftest            loopback self-test, needs no wiring
 *   myuart_test send TEXT...        write TEXT + newline
 *   myuart_test recv [SECONDS]      print what arrives (default 10 s)
 *   myuart_test echo                send every received byte back
 *   myuart_test stats               print driver counters
 *   myuart_test config [BAUD [BITS [N|O|E [STOP]]]]
 *   myuart_test loopback on|off
 *   myuart_test flush | clear
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../include/myuart_ioctl.h"

#define DEFAULT_DEV "/dev/myuart0"

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int dev_open(const char *path, int flags)
{
	int fd = open(path, flags);

	if (fd < 0)
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
	return fd;
}

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* deterministic byte stream so lost, repeated or reordered bytes show up */
static uint8_t next_byte(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return (uint8_t)(x >> 8);
}

/*
 * Push `total` pseudo-random bytes out and check that they come back in
 * order (loopback). One non-blocking fd, driven by poll(), so it works even
 * when `total` is far bigger than both kernel FIFOs.
 * Returns 0 on success, 1 on data mismatch, -1 on timeout or I/O error.
 */
static int transfer_verify(int fd, size_t total, unsigned timeout_ms,
			   char *why, size_t why_len)
{
	uint32_t tx_state = 0x1234abcd, rx_state = 0x1234abcd;
	uint8_t txbuf[256], rxbuf[256];
	size_t generated = 0, received = 0, pending = 0, pend_off = 0;
	uint64_t deadline = now_ms() + timeout_ms;

	while (received < total) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		uint64_t now = now_ms();
		int r;

		if (now >= deadline) {
			snprintf(why, why_len, "timeout: got %zu of %zu bytes",
				 received, total);
			return -1;
		}
		if (pending || generated < total)
			p.events |= POLLOUT;

		r = poll(&p, 1, (int)(deadline - now));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			snprintf(why, why_len, "poll: %s", strerror(errno));
			return -1;
		}
		if (r == 0)
			continue;	/* loop re-checks the deadline */

		if (p.revents & POLLOUT) {
			ssize_t w;

			if (!pending) {
				size_t n = total - generated;

				if (n > sizeof(txbuf))
					n = sizeof(txbuf);
				for (size_t i = 0; i < n; i++)
					txbuf[i] = next_byte(&tx_state);
				generated += n;
				pending = n;
				pend_off = 0;
			}
			w = write(fd, txbuf + pend_off, pending);
			if (w > 0) {
				pend_off += (size_t)w;
				pending -= (size_t)w;
			} else if (w < 0 && errno != EAGAIN && errno != EINTR) {
				snprintf(why, why_len, "write: %s", strerror(errno));
				return -1;
			}
		}

		if (p.revents & POLLIN) {
			ssize_t n = read(fd, rxbuf, sizeof(rxbuf));

			if (n > 0) {
				for (ssize_t i = 0; i < n; i++) {
					if (rxbuf[i] != next_byte(&rx_state)) {
						snprintf(why, why_len,
							 "mismatch at byte %zu",
							 received + (size_t)i);
						return 1;
					}
				}
				received += (size_t)n;
			} else if (n < 0 && errno != EAGAIN && errno != EINTR) {
				snprintf(why, why_len, "read: %s", strerror(errno));
				return -1;
			}
		}
	}
	return 0;
}

static int g_fails;

static void report(const char *name, int ok, const char *detail)
{
	printf("  [%s] %-44s %s\n", ok ? "PASS" : "FAIL", name,
	       (!ok && detail && *detail) ? detail : "");
	if (!ok)
		g_fails++;
}

static int cmd_selftest(const char *path, unsigned baud)
{
	static const size_t sizes[] = { 1, 16, 17, 255, 4096, 20000 };
	struct myuart_cfg want = { baud, 8, MYUART_PARITY_NONE, 1, 0 };
	struct myuart_cfg odd = { 57600, 7, MYUART_PARITY_EVEN, 2, 0 };
	struct myuart_cfg bad = { 1, 8, MYUART_PARITY_NONE, 1, 0 };
	struct myuart_cfg got;
	struct myuart_stats st;
	uint64_t expected = 0;
	char why[96], name[64];
	int on = 1, off = 0, fd, fd2;
	int ok;

	alarm(120);		/* never hang a boot script */
	fd = dev_open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return 1;

	printf("myuart selftest on %s (loopback, %u baud)\n", path, baud);

	ok = ioctl(fd, MYUART_IOC_SET_CFG, &odd) == 0 &&
	     ioctl(fd, MYUART_IOC_GET_CFG, &got) == 0 &&
	     got.baud == 57600 && got.data_bits == 7 &&
	     got.parity == MYUART_PARITY_EVEN && got.stop_bits == 2;
	report("config set/get round trip (57600 7E2)", ok, "");

	errno = 0;
	ok = ioctl(fd, MYUART_IOC_SET_CFG, &bad) == -1 && errno == EINVAL;
	report("invalid baud rejected with EINVAL", ok, "");

	ok = ioctl(fd, MYUART_IOC_SET_CFG, &want) == 0 &&
	     ioctl(fd, MYUART_IOC_LOOPBACK, &on) == 0 &&
	     ioctl(fd, MYUART_IOC_FLUSH) == 0 &&
	     ioctl(fd, MYUART_IOC_CLR_STATS) == 0;
	report("select line settings, loopback on, flush", ok, strerror(errno));
	if (!ok)
		goto out;

	{
		char c;
		struct pollfd p = { .fd = fd, .events = POLLIN };

		errno = 0;
		ok = read(fd, &c, 1) == -1 && errno == EAGAIN;
		report("O_NONBLOCK read on empty fifo -> EAGAIN", ok, "");
		ok = poll(&p, 1, 100) == 0;
		report("poll() times out while idle", ok, "");
	}

	/* blocking read/write on a second descriptor */
	fd2 = dev_open(path, O_RDWR);
	if (fd2 >= 0) {
		static const char msg[] = "hello myuart";
		char buf[32] = { 0 };
		size_t got_n = 0;

		ok = write_all(fd2, msg, sizeof(msg) - 1) == 0;
		while (ok && got_n < sizeof(msg) - 1) {
			ssize_t n = read(fd2, buf + got_n,
					 sizeof(msg) - 1 - got_n);

			if (n <= 0)
				ok = 0;
			else
				got_n += (size_t)n;
		}
		ok = ok && memcmp(buf, msg, sizeof(msg) - 1) == 0;
		report("blocking write + read round trip", ok, "");
		expected += sizeof(msg) - 1;
		close(fd2);
	}

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		size_t n = sizes[i];
		unsigned t_ms = 2000 + (unsigned)(n * 10ull * 1000 * 3 / baud);
		int r;

		why[0] = 0;
		r = transfer_verify(fd, n, t_ms, why, sizeof(why));
		snprintf(name, sizeof(name), "stream %zu bytes, data intact", n);
		report(name, r == 0, why);
		if (r == 0)
			expected += n;
		else
			goto out;	/* later counters would be meaningless */
	}

	ok = ioctl(fd, MYUART_IOC_GET_STATS, &st) == 0;
	if (ok) {
		ok = st.tx_bytes == expected && st.rx_bytes == expected;
		snprintf(why, sizeof(why), "tx=%llu rx=%llu expected=%llu",
			 (unsigned long long)st.tx_bytes,
			 (unsigned long long)st.rx_bytes,
			 (unsigned long long)expected);
		report("tx/rx byte counters match", ok, why);

		ok = st.sw_overruns == 0 && st.hw_overruns == 0 &&
		     st.parity_errors == 0 && st.framing_errors == 0 &&
		     st.breaks == 0;
		snprintf(why, sizeof(why),
			 "sw_ovr=%u hw_ovr=%u par=%u frm=%u brk=%u",
			 st.sw_overruns, st.hw_overruns, st.parity_errors,
			 st.framing_errors, st.breaks);
		report("no overruns or line errors", ok, why);
		printf("  (%u interrupts for %llu bytes each way)\n",
		       st.irq_count, (unsigned long long)expected);
	} else {
		report("read statistics", 0, strerror(errno));
	}

out:
	ioctl(fd, MYUART_IOC_LOOPBACK, &off);
	ioctl(fd, MYUART_IOC_FLUSH);
	close(fd);

	if (g_fails)
		printf("MYUART_SELFTEST_FAIL (%d failed)\n", g_fails);
	else
		printf("MYUART_SELFTEST_PASS\n");
	return g_fails ? 1 : 0;
}

static int cmd_send(const char *path, int argc, char **argv)
{
	int fd = dev_open(path, O_WRONLY);

	if (fd < 0)
		return 1;
	for (int i = 0; i < argc; i++) {
		if (i && write_all(fd, " ", 1))
			goto err;
		if (write_all(fd, argv[i], strlen(argv[i])))
			goto err;
	}
	if (write_all(fd, "\n", 1))
		goto err;
	close(fd);
	return 0;
err:
	perror("write");
	close(fd);
	return 1;
}

static int cmd_recv(const char *path, unsigned secs)
{
	int fd = dev_open(path, O_RDONLY | O_NONBLOCK);
	uint64_t deadline = now_ms() + secs * 1000ull;
	unsigned long total = 0;

	if (fd < 0)
		return 1;
	fprintf(stderr, "listening for %u s ...\n", secs);

	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		uint64_t now = now_ms();
		char buf[256];
		ssize_t n;

		if (now >= deadline)
			break;
		if (poll(&p, 1, (int)(deadline - now)) <= 0)
			continue;
		n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			if (write_all(STDOUT_FILENO, buf, (size_t)n))
				break;
			total += (unsigned long)n;
		}
	}
	fprintf(stderr, "\n%lu bytes received\n", total);
	close(fd);
	return 0;
}

static int cmd_echo(const char *path)
{
	int fd = dev_open(path, O_RDWR);
	char buf[256];

	if (fd < 0)
		return 1;
	fprintf(stderr, "echoing on %s (Ctrl-C to stop)\n", path);
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			break;
		}
		write_all(STDOUT_FILENO, buf, (size_t)n);
		if (write_all(fd, buf, (size_t)n)) {
			perror("write");
			break;
		}
	}
	close(fd);
	return 1;
}

static int cmd_stats(const char *path)
{
	struct myuart_stats s;
	struct myuart_cfg c;
	int fd = dev_open(path, O_RDONLY);

	if (fd < 0)
		return 1;
	if (ioctl(fd, MYUART_IOC_GET_STATS, &s) || ioctl(fd, MYUART_IOC_GET_CFG, &c)) {
		perror("ioctl");
		close(fd);
		return 1;
	}
	printf("line            %u baud, %u data bits, parity %s, %u stop%s\n",
	       c.baud, c.data_bits,
	       c.parity == MYUART_PARITY_NONE ? "none" :
	       c.parity == MYUART_PARITY_ODD ? "odd" : "even", c.stop_bits,
	       s.loopback ? ", loopback ON" : "");
	printf("rx_bytes        %llu\n", (unsigned long long)s.rx_bytes);
	printf("tx_bytes        %llu\n", (unsigned long long)s.tx_bytes);
	printf("irq_count       %u\n", s.irq_count);
	printf("rx_fifo_level   %u\n", s.rx_fifo_level);
	printf("tx_fifo_level   %u\n", s.tx_fifo_level);
	printf("sw_overruns     %u\n", s.sw_overruns);
	printf("hw_overruns     %u\n", s.hw_overruns);
	printf("parity_errors   %u\n", s.parity_errors);
	printf("framing_errors  %u\n", s.framing_errors);
	printf("breaks          %u\n", s.breaks);
	close(fd);
	return 0;
}

static int cmd_config(const char *path, int argc, char **argv)
{
	struct myuart_cfg c;
	int fd = dev_open(path, O_RDWR);

	if (fd < 0)
		return 1;
	if (ioctl(fd, MYUART_IOC_GET_CFG, &c)) {
		perror("ioctl GET_CFG");
		close(fd);
		return 1;
	}
	if (argc >= 1)
		c.baud = (unsigned)strtoul(argv[0], NULL, 0);
	if (argc >= 2)
		c.data_bits = (uint8_t)strtoul(argv[1], NULL, 0);
	if (argc >= 3) {
		switch (argv[2][0]) {
		case 'n': case 'N': c.parity = MYUART_PARITY_NONE; break;
		case 'o': case 'O': c.parity = MYUART_PARITY_ODD;  break;
		case 'e': case 'E': c.parity = MYUART_PARITY_EVEN; break;
		default:
			fprintf(stderr, "parity must be N, O or E\n");
			close(fd);
			return 1;
		}
	}
	if (argc >= 4)
		c.stop_bits = (uint8_t)strtoul(argv[3], NULL, 0);

	if (argc >= 1 && ioctl(fd, MYUART_IOC_SET_CFG, &c)) {
		fprintf(stderr, "SET_CFG: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	ioctl(fd, MYUART_IOC_GET_CFG, &c);
	printf("%u %u%c%u\n", c.baud, c.data_bits,
	       c.parity == MYUART_PARITY_NONE ? 'N' :
	       c.parity == MYUART_PARITY_ODD ? 'O' : 'E', c.stop_bits);
	close(fd);
	return 0;
}

static int cmd_simple_ioctl(const char *path, unsigned long req, int *arg)
{
	int fd = dev_open(path, O_RDWR);
	int r;

	if (fd < 0)
		return 1;
	r = arg ? ioctl(fd, req, arg) : ioctl(fd, req);
	if (r)
		perror("ioctl");
	close(fd);
	return r ? 1 : 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-d DEVICE] [-b BAUD] COMMAND [ARGS]\n\n"
		"  selftest                 loopback self-test (no wiring needed)\n"
		"  send TEXT...             write TEXT and a newline\n"
		"  recv [SECONDS]           print incoming data (default 10 s)\n"
		"  echo                     send every received byte back\n"
		"  stats                    show driver counters\n"
		"  config [BAUD [BITS [N|O|E [STOP]]]]   show or set the line\n"
		"  loopback on|off          internal UART loopback\n"
		"  flush                    drop queued RX/TX data\n"
		"  clear                    zero the counters\n\n"
		"  -d DEVICE   default %s\n"
		"  -b BAUD     baud rate used by selftest (default 115200)\n",
		prog, DEFAULT_DEV);
}

int main(int argc, char **argv)
{
	const char *prog = argv[0];
	const char *dev = DEFAULT_DEV;
	unsigned baud = 115200;
	const char *cmd;
	int opt;

	while ((opt = getopt(argc, argv, "+d:b:h")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'b': baud = (unsigned)strtoul(optarg, NULL, 0); break;
		default:  usage(argv[0]); return opt == 'h' ? 0 : 2;
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
		return 2;
	}
	cmd = argv[optind++];
	argc -= optind;
	argv += optind;

	if (!strcmp(cmd, "selftest"))
		return cmd_selftest(dev, baud);
	if (!strcmp(cmd, "send"))
		return cmd_send(dev, argc, argv);
	if (!strcmp(cmd, "recv"))
		return cmd_recv(dev, argc ? (unsigned)strtoul(argv[0], NULL, 0) : 10);
	if (!strcmp(cmd, "echo"))
		return cmd_echo(dev);
	if (!strcmp(cmd, "stats"))
		return cmd_stats(dev);
	if (!strcmp(cmd, "config"))
		return cmd_config(dev, argc, argv);
	if (!strcmp(cmd, "loopback") && argc == 1) {
		int on = !strcmp(argv[0], "on");

		return cmd_simple_ioctl(dev, MYUART_IOC_LOOPBACK, &on);
	}
	if (!strcmp(cmd, "flush"))
		return cmd_simple_ioctl(dev, MYUART_IOC_FLUSH, NULL);
	if (!strcmp(cmd, "clear"))
		return cmd_simple_ioctl(dev, MYUART_IOC_CLR_STATS, NULL);

	usage(prog);
	return 2;
}
