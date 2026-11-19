# Polaris touch input

This driver exposes the MIX 2S Synaptics RMI4 F01/F12 controller through UEFI
Absolute Pointer. It discovers the coordinate limits and F12 register layout
instead of assuming a fixed report address. It reports one active contact and
preserves press/release transitions while coalescing movement events.

The board uses slave address `0x20` on SE14 (firmware core 15): SDA GPIO33,
SCL GPIO34, IRQ GPIO125, reset GPIO99, and fixed 1.8 V VIO enable GPIO23.
The inherited display boot chain supplies L28A. Initialization checks that
supply without rewriting PMIC settings; L6A is not a touchscreen supply.

On the tested firmware, `GENI_IF_DISABLE_RO` disables the FIFO interface and
selects GPI DMA. The inherited EFI I2C implementation uses the FIFO interface,
so this driver currently uses dedicated GPIO pins for a bounded open-drain
I2C transport. High levels release the pins instead of actively driving them.
It supports repeated START, final-byte NACK, clock stretching, and a transfer
deadline. Initialization failure and ExitBootServices restore function 2 on
both pins for the operating system's GPI driver.

Polling uses a relative timer rearmed after each transfer. A periodic timer
can repeatedly dispatch overdue callbacks when a GPIO transfer exceeds its
interval, preventing the UI and USB service from running. Repeated transfer
errors stop polling and release a held contact.

F01 interrupt status is read before each sample. Initialization enables and
verifies F12's absolute-data interrupt without clearing other functions' enable
bits. Idle polls skip the object report. A sample first reads the first contact;
if that slot is inactive, it reads all slots to find another contact or a release.
A failed object read remains pending after F01 has acknowledged the interrupt.

`fastboot oem firmware-log` reports `POLARIS_TOUCH ready` after controller
discovery. On hardware this reports limits `1079,2159` and 80 report bytes.
That message establishes initialization; actual pointer events and UI
interaction require a separate touch test. Host tests cover RMI descriptor
bounds, press/release handling, timer failure, wire framing, NACK, timeout,
open-drain behavior, and pin restoration.
