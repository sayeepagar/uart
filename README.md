# myuart: an interrupt-driven UART character driver for Linux

A Linux kernel module that drives a 16550A UART and exposes it as `/dev/myuart0`.
It runs on QEMU with no hardware needed. It comes with a user-space test tool, a
one-command QEMU demo and an automated self-test.

- Interrupt-driven RX and TX, 4 KiB kernel FIFOs in each direction
- `read`/`write` with blocking and `O_NONBLOCK` semantics, plus `poll`/`select`/`epoll`
- `ioctl`: baud rate, data bits, parity, stop bits, internal loopback, flush, statistics
- `/proc/myuart` with live counters
- Character-device plumbing done by hand: `alloc_chrdev_region`, `cdev`, `class`, `device`

## Architecture

```
 user space   myuart_test ── open/read/write/poll/ioctl ──┐
                                                           │
 kernel                    ┌──────────── myuart.ko ────────▼──────────┐
                           │  read()  ◄── rx_fifo (4 KiB) ◄──┐        │
                           │  write() ──► tx_fifo (4 KiB) ──┐│  ISR   │
                           └────────────────────────────────┼┼────────┘
                                                            ▼│
 hardware         16550A UART  (QEMU: COM2, port 0x2f8, IRQ 3)
                  └── to the host over TCP  (or to a real board, see docs/STM32_PORT.md)
```

QEMU's first serial port (COM1) stays the Linux console. The second (COM2) belongs
to this driver.

## Quick start

Needs a Linux host (Ubuntu, or Ubuntu under WSL2).

```bash
sudo apt install build-essential flex bison bc libssl-dev libelf-dev xz-utils curl \
                 qemu-system-x86 busybox-static cpio

make kernel     # one time, a few minutes: builds a tiny kernel into build/linux
make test       # builds everything, boots QEMU, runs the self-test, prints PASS/FAIL
make run        # same build, but interactive: you get a shell inside the VM
```

`make kernel` is the recommended path: the module is built against exactly the
kernel QEMU boots, so there is nothing to mismatch.

**Using your host kernel instead** (skip `make kernel`): install
`linux-headers-$(uname -r)` and make the image readable
(`sudo chmod a+r /boot/vmlinuz-$(uname -r)`). `make` and `make run` then use the host
kernel. This path is less predictable; if something odd happens, use `make kernel`.

### What a passing run looks like

```
  [PASS] config set/get round trip (57600 7E2)
  [PASS] invalid baud rejected with EINVAL
  [PASS] O_NONBLOCK read on empty fifo -> EAGAIN
  [PASS] poll() times out while idle
  [PASS] blocking write + read round trip
  [PASS] stream 1 / 16 / 17 / 255 / 4096 / 20000 bytes, data intact
  [PASS] tx/rx byte counters match
  [PASS] no overruns or line errors
MYUART_SELFTEST_PASS
```

The streams use a pseudo-random sequence, so a lost, repeated or reordered byte
fails the test. The 20000-byte stream is larger than both kernel FIFOs and
exercises the full-FIFO and wake-up paths.

## Talking to the host

`make run` opens a shell in the VM and exposes COM2 as TCP port 5555 on the host.

```
host terminal 2:   nc 127.0.0.1 5555
in the VM:         myuart_test recv 60        # type in the nc window: text appears
                   myuart_test send hello     # appears in the nc window
                   myuart_test echo           # host-typed text is echoed back
                   cat /proc/myuart           # counters
```

Quit QEMU with `Ctrl-A` then `X`.

## Using the device from C

```c
#include "include/myuart_ioctl.h"

int fd = open("/dev/myuart0", O_RDWR);
struct myuart_cfg cfg = { .baud = 9600, .data_bits = 8,
                          .parity = MYUART_PARITY_NONE, .stop_bits = 1 };
ioctl(fd, MYUART_IOC_SET_CFG, &cfg);
write(fd, "hi\n", 3);
```

| ioctl | argument | effect |
|---|---|---|
| `MYUART_IOC_SET_CFG` / `GET_CFG` | `struct myuart_cfg` | baud 300..115200, 5..8 data bits, N/O/E parity, 1..2 stop |
| `MYUART_IOC_GET_STATS` | `struct myuart_stats` | byte, IRQ, overrun and error counters; FIFO levels |
| `MYUART_IOC_CLR_STATS` | none | zero the counters |
| `MYUART_IOC_FLUSH` | none | drop queued RX and TX data |
| `MYUART_IOC_LOOPBACK` | `int` 0/1 | UART internal loopback (TX feeds RX) |

## How it works

| Concern | Choice | Why |
|---|---|---|
| FIFOs | `kfifo`, one producer and one consumer per direction | Lock-free by construction: ISR→`read()` for RX, `write()`→ISR for TX |
| Register access, IER/MCR shadows, config, stats | one IRQ-safe `spinlock_t`, also taken by the ISR | the ISR and process context share the registers |
| Serialising readers and writers | two mutexes | `copy_to_user`/`copy_from_user` can sleep, so they must not run under a spinlock |
| Baud change | done under the spinlock | while DLAB=1, offsets 0/1 are the divisor latches, not RBR/THR; the ISR must not run then |
| TX start | `write()` queues bytes, then sets the THRE interrupt enable; the ISR sends up to 16 bytes per interrupt and disables it when the FIFO is empty | enabling *after* queuing means a byte can never be stranded |
| Blocking I/O | two wait queues + `wait_event_interruptible` | `read()` sleeps until data arrives, `write()` until there is room; signals interrupt cleanly |
| Bounded ISR | at most 256 loop iterations per interrupt | a misbehaving UART cannot lock the CPU |
| Damaged bytes | parity/framing errors and breaks are counted and dropped | keeps the data stream clean; counters tell you it happened |
| Portability | all port I/O goes through `myuart_in()`/`myuart_out()` | an MMIO version only has to change those two functions |
| Kernel versions | `class_create()` signature guarded with `LINUX_VERSION_CODE` | the API changed in 6.4 |

## Files

```
driver/myuart.c           the kernel module
include/myuart_ioctl.h    user/kernel ABI (shared by driver and app)
app/myuart_test.c         selftest, send, recv, echo, stats, config, loopback, flush
scripts/build_kernel.sh   optional: tiny kernel into build/linux
scripts/build_initramfs.sh, init.sh, run_qemu.sh
docs/STM32_PORT.md        plan for the STM32 Cortex-M4 board
```

## Tested on

- Linux 6.6 (tinyconfig-based), QEMU 8.2 with software emulation (no KVM), x86_64
- Module compiles without warnings; the self-test passes 14/14 (24 KB streamed in each direction); host
  to guest and guest to host over the TCP serial port both verified
- **Not tested:** other kernel versions, KVM, the host-kernel path, real hardware

## Limitations

- One device instance, x86 port I/O only
- Raw character device, not part of the kernel's tty/serial layer. Production UART
  drivers register with `uart_driver`/`uart_ops` and get termios, line disciplines and
  `/dev/ttyS*` semantics for free. This project is a deliberate step below that, to
  learn the character-driver and interrupt mechanics.
- With `MCR` loopback on **real** hardware the UART disconnects its IRQ line, so the
  self-test needs QEMU, which does not model that.

## Next steps

- STM32 Cortex-M4 board: see [docs/STM32_PORT.md](docs/STM32_PORT.md)
- Turn it into a `platform_driver` with device-tree MMIO binding (`ioremap`, `readb`/`writeb`)
- Register with the serial core (`uart_driver`) instead of a raw cdev
- Add DMA, and a kselftest-style test

## Interview talking points

- Why the kfifos need no lock (single producer, single consumer) and what each lock
  *does* protect
- The lost-wakeup race in TX start and why queue-then-enable closes it
- Why `copy_to_user` can't run under a spinlock
- 16550 FIFO trigger levels and the receive-timeout interrupt
- What differs from a production serial driver (above) and how you would close the gap

## Resume line

> Developed an interrupt-driven Linux character driver for a 16550A UART: lock-free
> kfifo RX/TX paths, blocking and non-blocking I/O with poll support, ioctl line
> configuration and `/proc` statistics; validated with an automated QEMU self-test
> (streaming loopback with data verification, zero overruns).

Add your own numbers once you measure them.

The module is licensed GPL-2.0 (`MODULE_LICENSE("GPL")`).
