# Polaris recovery acceptance requirements

The current Renegade-based image is a buildable baseline, not a complete
recovery implementation. No device validation has been performed.

| Mechanism | Required behavior | Current implementation |
| --- | --- | --- |
| USB fastboot | Device-mode enumeration; bounded download; deterministic disconnect/reset handling | Standard USBFn implementation; query/download host tests pass; hardware untested |
| Partition flashing | Resolve GPT names; enforce bounds; raw and Android sparse images; flush and report I/O failures | See DEBUGGER.md for implemented full-raw/readback and mapped-RAM paths |
| RAM read | Explicit address/length protocol; overflow checks; memory-map validation; reject MMIO and inaccessible ranges | See DEBUGGER.md for implemented full-raw/readback and mapped-RAM paths |
| Watchdog | Hardware timeout, progress-based feeding, stop/rearm across boot transitions; recover from stalled operations | KPSS hardware implementation added; real expiry remains untested |
| USB recovery | Enter without a working rootfs; remain reachable after failed boot; reconnect after transfer failure | Built-in entry, reset invalidation, transfer timeout/restart; hardware untested |
| Temporary boot | Validate downloaded images; boot from RAM without partition writes; retain recovery after failure | AArch64 EFI application download/LoadImage/StartImage and return-to-USB implemented; Android boot images unsupported |

Validation must cover malformed commands, oversized downloads, sparse-image
overflow, out-of-partition writes, partial storage failures, cable removal,
watchdog expiry, and repeated temporary boots. Host tests do not establish USB,
UFS, reset, or watchdog operation on Polaris hardware.

Android EFI reads the ROM boot image and native device tree from the ESP, then
invokes the SimpleInit ARM64 loader. The EFI accepts legacy v0/v1 boot images
with gzip or raw kernels supported by SimpleInit and uses a matched board DTB.
This path does not select a firmware-embedded Android kernel. RAM preflight
and actual Android boot are separate device tests. Windows EFI chainloads an existing Windows boot
manager; it does not provision Windows or its board drivers.

## First hardware milestone

Default `build.py --entry pocketjs` starts the real shared runtime with the resident
fastboot backend; see [debugger details](DEBUGGER.md).

For the standalone recovery application, build with `build.py --entry recovery` and the initialized platform arguments.
The image enters built-in recovery after connecting drivers; returning from
recovery opens SimpleInit. `--entry menu` starts SimpleInit directly. Neither
entry path changes persistent BootOrder. Select recovery in the menu to retry.

Failed built-in entry launches now stay on `POLARIS_ENTRY_DIAG`, displaying
the source identity, application GUID, FV-read/load/application-return stage
and EFI status. Volume UP/R retries the entry; Volume DOWN/M selects the menu;
ESC/S explicitly opens UEFI Shell. An error does not silently resume BootOrder
and enter Shell. USB initialization also prints its operation and return status.
These diagnostics distinguish the fallback symptom from the underlying failure;
the first hardware report of `Shell>` alone did not identify that failure.

The service currently uses USB VID/PID `18d1:d00d`, fastboot interface
`ff/42/03`, and the temporary serial `polaris-uefi`. Test one Polaris at a time.
Downloads are limited to 64 MiB. Detach/reset invalidates downloaded content;
30 seconds without download/TX progress restarts the controller. This timeout
depends on the firmware event loop and does not replace a hardware watchdog.

After entering **this UEFI recovery**, run:

```sh
fastboot -s polaris-uefi getvar polaris-boot-format
fastboot -s polaris-uefi stage output/tools/return-probe.efi oem boot-efi
fastboot -s polaris-uefi getvar product
```

Expected: format `efi-aa64`, the probe's two on-screen markers, then recovery
USB reappears. Repeat three times. `stage` downloads without adding an Android
boot header, as implemented by the [AOSP fastboot client](https://android.googlesource.com/platform/system/core/+/master/fastboot/fastboot.cpp).
`fastboot boot` commonly packages an Android boot image; use the explicit
`stage ... oem boot-efi` sequence for this milestone.

`tests/device_smoke.py` automates the sequence and accepts an external report
path. It verifies the custom recovery's format before downloading a probe and
issues no partition-write commands. It has not yet run on a device. Separately
test cable removal during download and confirm that a new connection cannot
boot stale/partial data.

Native tests (x86-64 host with a compiler and EDK II headers):

```sh
python3 tests/run.py --edk2 <initialized-edk2> --output <test-build-dir> --sanitize
```

The first device entry from stock fastboot remains a separate board-level
validation: the recovery wrapper has been built and structurally checked, but
stock bootloader acceptance, display, USB role and driver initialization are
unverified. No partition flashing is needed for the RAM EFI probe itself.
