# Taking myuart to an STM32 Cortex-M4 board

## Read this first

An STM32F4-class chip (Cortex-M4) has no MMU, so it cannot run this Linux kernel
module. "Porting to the STM32" therefore means two different things. Both are
useful, and they are independent:

- **Step 1: use the board as the *peer*.** Real firmware on a real wire, with the
  Linux driver still running in QEMU. No new driver code.
- **Step 2: re-implement the same driver architecture as bare-metal or FreeRTOS
  firmware on the STM32.**

## Step 1: real board on the other end of the UART

Flash a trivial firmware on the board: USART at 115200 8N1, interrupt-driven echo
(every byte received is sent back). On a Nucleo board the on-board ST-LINK exposes
a USART to the host as a virtual COM port (`/dev/ttyACM0` on Linux). On a Nucleo-F446RE
that is USART2 on PA2/PA3; check your board's manual.

Then give that device to QEMU as COM2:

```bash
SERIAL2=/dev/ttyACM0 make run      # your user needs access: sudo usermod -aG dialout $USER
```

Inside the VM, `myuart_test send hello` goes through the Linux driver, out over QEMU, down
the USB cable, through the STM32 and back. `myuart_test recv 5` prints the echo.
This exercises real line timing and lets you provoke real framing errors (change
the board's baud rate and watch `framing_errors` and `parity_errors` rise in
`/proc/myuart`).

## Step 2: same design on the microcontroller

The architecture transfers one-to-one. Only the primitives change:

| myuart (Linux) | STM32F4 bare-metal / FreeRTOS |
|---|---|
| 16550 RBR / THR | `USART_DR` |
| LSR bits DR, THRE, OE, FE, PE | `USART_SR` bits RXNE, TXE, ORE, FE, PE |
| IER: RDI / THRI | `USART_CR1`: RXNEIE / TXEIE |
| divisor latch (DLL/DLM) | `USART_BRR` |
| LCR (bits, parity, stop) | `CR1` (M, PCE, PS) and `CR2` (STOP) |
| `MCR` loopback | none on F4 USART: jumper TX to RX instead |
| `request_irq` + ISR | `NVIC_EnableIRQ(USARTx_IRQn)` + `USARTx_IRQHandler` |
| `kfifo` | ring buffer, power-of-two size, one writer/one reader index (`volatile`) |
| `spin_lock_irqsave` | `PRIMASK`/`BASEPRI` critical section (`taskENTER_CRITICAL`) |
| wait queue + `wait_event_interruptible` | binary semaphore / task notification, given with `...FromISR` |
| `poll()` | event group or queue set |
| `ioctl` | `uart_set_cfg()`, `uart_get_stats()` functions |
| `/proc/myuart` | a CLI command or a debug struct |

Things that differ in practice:

- **ISR priority and FreeRTOS.** An ISR that calls `...FromISR` functions must have
  a priority numerically at or above `configMAX_SYSCALL_INTERRUPT_PRIORITY`.
- **Clearing errors.** On the F4, an overrun (ORE) is cleared by reading `SR` then
  `DR`, in that order. The 16550 clears error bits by reading LSR.
- **TXE vs TC.** TXE means "data register empty", not "last bit left the wire";
  use TC if you need to know the line is idle (RS-485 direction control, for example).
- **No FIFO.** F4 USARTs have none, so the RX ISR must keep up byte by byte; this is
  why the ring buffer and short ISR matter more here than on the 16550.

Suggested milestones: (1) polled echo, (2) interrupt RX into a ring buffer, (3)
interrupt TX, (4) FreeRTOS blocking `read`/`write` with timeouts, (5) a stats
struct and a command to print it, (6) the same selftest idea: loopback with a
pseudo-random stream and byte-for-byte verification.
