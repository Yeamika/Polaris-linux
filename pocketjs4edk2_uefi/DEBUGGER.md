# Polaris PJ4EDK2 and fastboot debugger

The normal entry is the real PJ4EDK2 EFI runtime, not a renamed SimpleInit menu.
It uses the shared runtime and default UI from the initialized `components/pj4edk2`
checkout. System cards launch `\\EFI\\<system>\\bootaa64.efi` from the ESP, matching
the W202DS menu pattern. The ESP menu at `EFI/BOOT/pj4edk/config.json` overrides
the built-in defaults; merge existing preferences when deploying a new menu.
SimpleInit remains an explicit firmware-volume fallback. Its session menu disables
automatic startup without saving device config. Discovery excludes PocketJS itself.

The resident DXE backend implements the shared `PJ4EDK2_FASTBOOT_PROTOCOL`,
using Qualcomm USBFn rather than the W202DS LK gateway. Polling is bounded to eight
USB events/storage chunks per UI frame at application TPL. A returning EFI releases
USB first; the backend restarts afterwards. Initial USB discovery retries once per
second; reset/detach and transfer failure invalidate downloaded data. Failed USB
stop retains DMA ownership and prevents handoff.

While the resident backend is active, the existing UEFI ConOut and StdErr
OutputString interfaces route text into the current-boot RAM log (`oem log`)
instead of drawing over the graphical UI. This includes returning EFI console
messages. The original functions are restored on successful backend stop and
ExitBootServices. Direct framebuffer drawing and serial debug output are separate
interfaces; the standalone recovery diagnostic retains its screen console.

On the pinned SDM845 firmware, USBFn IO is published by a driver bound to the
peripheral controller handle, rather than by merely loading its DXE image. When
USBFn is absent, recovery requests core 0, device mode 4 through Qualcomm
USBConfig revision `0x10003`, then repeats USBFn discovery. The minimal adapter
rejects other ABI revisions; it does not patch the binary or write USB policy
variables. Device-mode startup and command exchange have been verified on Polaris.

For USB initialization diagnosis, build with `--entry recovery` and temporarily
boot the resulting image with the original bootloader. The screen reports the
exact initialization stage, EFI status, USBFn revision, and event/setup/reset
counts. `configured=1` means the host completed USB configuration, not that a
partition write or RAM boot has been validated. This diagnostic entry uses the
SimpleInit fallback when explicitly leaving recovery; normal `--entry pocketjs`
continues to launch the real PJ4EDK2 runtime.

## W202DS debugger comparison

| Capability | Polaris implementation | Validation |
| --- | --- | --- |
| Real PocketJS UI with fastboot backend | PJ4EDK2 runtime plus resident Qualcomm USBFn backend | Hardware command exchange verified; touch input pending |
| Complete raw partition flash | Exact unique GPT name, full image size, aligned 64 KiB writes, flush and readback | Full 64 MiB boot self-flash and subsequent persistent boot verified; injected failures tested |
| Download only into RAM | Up to 64 MiB; reset invalidates data | Parser and USB transport tests |
| SDK partition queries | `has-slot:*` and `is-logical:*` return `no`; `current-slot` fails so callers must name the target explicitly | Parser regression tests |
| RAM read and CRC32 | 64-bit addresses, 16-byte alignment, at most 64 KiB per command; normal mapped RAM only | Hardware 4 KiB read with complete ordered packets and verified CRC |
| RAM module execution and return | AArch64 PE EFI (`stage` + `oem boot-efi`) | Three consecutive return cycles verified under PJ4EDK2 |
| Hardware watchdog | SDM845 KPSS at 32.768 kHz, 30 seconds; UI polls feed it; rearmed after ExitBootServices USB cleanup; kernel must take ownership before expiry | Post-EBS deliberate stall reset and fastboot recovery verified on Polaris; OS takeover pending |
| Reset recovery | UEFI boots from boot; service retries USB; native ABL retained | Ordinary reset observed; bootloader-targeted reset pending |
| Readable debug evidence | `oem log`, `oem memory-map`, readback/EFI return statuses | Host protocol tests; logs are current-boot RAM only |

W202DS's PLK1 modules and fixed memory addresses are not compatible with Polaris.
The comparison uses W202DS's resident `lk/legacy-buildchain/fastboot/service.c`,
not the unrelated vendor LK source tree. Both services accept fragmented downloads
and ignore USB zero-length packets during the data phase. Polaris additionally
checks that media identity, block size and partition capacity remain unchanged
before every write chunk; a change invalidates the downloaded image and stops I/O.
This implementation does not provide GDB single-stepping, arbitrary MMIO access,
sparse images, erase, or automatic slot selection. Android boot-image loading is
an experimental separate EFI adapter, not a fastboot RAM-boot format.
It does not claim W202DS reset-persistent RAM trace support. A successful build or
host test does not establish real USB/UFS, touch input, watchdog expiry or recovery.

Known reset limitation: the pinned platform uses PSCI ResetSystemLib, which ignores
the Qualcomm `RESET_PARAM` payload. `reboot bootloader` currently acknowledges and
performs an ordinary reset, returning to the installed boot image. It must not be
used as evidence of reaching native fastboot; board-specific reboot-reason support
is still required. Watchdog reset after an intentional post-EBS stall has been
verified; Android kernel takeover remains unverified.

## Commands

Use the custom service serial, not the original ABL serial:

```sh
fastboot -s polaris-uefi getvar product
fastboot -s polaris-uefi getvar polaris-debugger
fastboot -s polaris-uefi getvar partition-size:boot
fastboot -s polaris-uefi oem memory-map
fastboot -s polaris-uefi oem log
fastboot -s polaris-uefi oem firmware-log
fastboot -s polaris-uefi stage output/tools/return-probe.efi oem boot-efi
python3 output/tools/debug.py --serial polaris-uefi info
python3 output/tools/debug.py --serial polaris-uefi map
python3 output/tools/debug.py --serial polaris-uefi read \
  --address <mapped-aligned-address> --size 0x1000 --output memory.bin
python3 output/tools/debug.py --serial polaris-uefi probe \
  --efi output/tools/return-probe.efi --cycles 3
```

Before PJ4EDK2 takes the display, firmware DEBUG and UEFI ConOut/StdErr share one
top-left framebuffer console, font and scrolling cursor. GOP/text-console
initialization preserves this log. Once the UI starts, ordinary firmware output
is retained in a 16 KiB RAM ring instead of painting over the UI. `oem firmware-log`
reads a snapshot of that ring; `oem log` retains the separate USB/recovery log.
Non-text serial writes are kept in the ring but do not paint characters or move
the screen cursor. The early font uses a placeholder for unsupported Unicode.
The ring lasts for this firmware session only. Unexpected CPU exceptions still
take over the framebuffer. LinuxBoot returns ownership to the same top-left
renderer during setup and EFI stub output. Other child EFI and an explicitly
opened Shell use normal UEFI text output after the UI relinquishes the console.

Linux-specific tools are also built under `tools/`:

- `linux-prepare.efi` prepares the DT/display/initrd and loads the kernel EFI,
  then returns without calling the kernel entry point.
- `linux-trace.efi` runs the normal Linux preparation with explicit `efi=debug`
  and `earlycon=efifb` options. It checks the live memory map before recording an
  EBS journal in the final page of the reserved display region. The hook updates
  the boot-services CRC and performs no allocation or storage I/O during EBS.
  Phase 2 is EBS entry, 3 is successful return, 4 is a failed return, and 5 means
  the child EFI returned normally. Phase 3 alone does not prove kernel entry.
- `linux-inspect.efi` reads the ESP pre-EBS log and initramfs diagnostics, plus
  the journal if it is valid. It prints a page-aligned snapshot address and
  length for the checked RAM reader. The bounded data allocation lasts until
  reset; the tool does not boot a kernel or write storage.

`EFI/LinuxBoot/boot-status.txt` contains up to 32 KiB of preparation and stub
console output before EBS. The initramfs hook writes separate kernel logs only
after reaching userspace. Missing kernel logs cannot establish a successful
initramfs boot. Reset survival of the display-reservation journal still requires
hardware verification; do not infer it from a host test or a retained magic.

The RAM client verifies every address, packet order and per-chunk CRC, and writes
binary data, metadata and raw transport evidence. It refuses an existing output.
Use the memory-map command to select RAM; MMIO, unmapped, reserved and
read-protected memory are rejected. Output is a live read, not an atomic snapshot.

The supplied hang probe deliberately stalls. Run only as an explicit watchdog
hardware test; success requires observing service disappearance and recovery:

```sh
python3 output/tools/debug.py --serial polaris-uefi watchdog-test \
  --efi output/tools/hang-probe.efi
```

`output/tools/ebs-hang-probe.efi` deliberately stalls after a successful
`ExitBootServices`, to test early kernel handoff protection separately. It does
not boot an OS or write storage. A USB disconnect alone is not proof of reset:
observe the reboot and check the recovered service. `getvar:polaris-watchdog`
reports KPSS enable, status, bark and bite registers for diagnosis. The kernel
must take over the 30 second watchdog; OS takeover remains a hardware test gate.
The post-EBS stall test recovered USB fastboot about 60 seconds after launch,
including firmware startup and USB enumeration; this is not a measured 60 second
hardware timeout. Register sampling also confirmed the counter advances.

The Android card loads the selected ROM kernel, ramdisk and native board DTB
from the ESP. It does not need an Android BootShim binding in the installed
boot partition. `output/tools/android-prepare.efi` uses the same loader and
returns after preparing the payload, before kernel entry. Both read
`EFI/Android/boot.img` and `EFI/Android/board.dtb` and use request revision 3.
An EFI caller can supply these inputs from RAM using the same request.
The loader restores the previous EFI FDT table after a diagnostic return.
Preparation does not establish Android boot or OS watchdog takeover.

The Android handoff records 64 bytes at `0xcffff000`, a page protected from kernel
allocation by a DT reservation. The checked RAM client can read the current boot's
record. Ordinary reset overwrites this region on Polaris, so the recovered record
must not be treated as evidence of the preceding boot's final stage.
The eight little-endian 64-bit fields are magic (`0x335243544c4f5055`), stage,
kernel address, DTB address, initrd address, CurrentEL, status and a reserved field.
Stage `0x10` means preparation passed; `0x31` precedes ExitBootServices, `0x40`
follows it, `0x50` enters assembly and `0x54` immediately precedes the kernel
branch with MMU disabled. A stage record alone does not prove Android userspace
or reliable retention across every kind of reset. This Android-specific record
does not provide W202DS's general persistent debugger trace.

A `flash:<name>` accepts only a raw image whose size exactly equals the named GPT
partition. This matches W202DS's full-image/readback policy. Sparse and partial
images are rejected before any write; failed or disconnected writes may leave an
incomplete partition and are never automatically retried. No flash command is run
by the debugger's info, map, read, log or probe operations. Explicit names do not
select aliases or synchronize another slot. ABL/XBL are untouched by installing
the UEFI image into boot with the original factory fastboot.

Android EFI prepares the original kernel, ramdisk and native device tree, then
uses a stackless raw ARM64 handoff. Android userspace through this route remains
unverified; the native bootloader route must be validated separately.
Windows EFI requires an existing boot manager; LinuxBoot requires a compatible
kernel, DTB and userspace. Those payloads are not installed by this debugger.

### Linux native handoff update (2026-11-15)

The Polaris ESP Linux launcher now shares the native ARM64 entry with the
explicit native probes. Device checkpoints confirmed `run_init_process`, the
first EL0 syscall, and the mount syscall. This establishes initramfs userspace
execution; it does not establish working USB/UFS or server startup.
The QEMU full-init and syscall checkpoint records are under the external
Polaris build directory. The standard EFI stub remains an explicit trace tool.

Sealing the ESP log preserves the bounded screen renderer through EBS. Later
EBS callbacks must not restore the firmware cursor or display ownership. RAM
launches without a BootDisplay table use the full-screen fallback, while menu
launches consume the card's actual rectangle; use the latter to validate UI
handoff geometry. The preparation log reports the selected geometry.
