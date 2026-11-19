# Polaris Windows EFI

`Windows.efi` prepares Polaris and starts `\EFI\Microsoft\Boot\bootmgfw.efi`
on the selected ESP. Install it as `EFI/Windows/BOOTAA64.EFI` together with the
matching firmware; Windows handoff policy revision 2 and the bounded kernel
errata driver are required.

The firmware retains BootShim, PrePi, the memory map, Qualcomm DXE drivers,
SMBIOS, ACPI publication and the Windows kernel errata driver. The launcher
does the preparation bypassed by the early PJ4EDK2 menu:

1. Stop the resident USB service and Rotator DMA, requiring successful release.
2. Read real ChipInfo, PlatformInfo and SMEM item 137. Validate reserved SMEM,
   TCM, remote filesystem and framebuffer ranges against the EFI memory map.
3. Fill the fixed Polaris DSDT Name objects in a checked copy, then replace the
   table once. Preserve AML integer widths, update its checksum and verify the
   FADT points to the prepared DSDT. A failed replacement attempts rollback.
4. Install the pinned complete SDM845 CSRT if absent; reject incompatible or
   duplicate tables. Signal ReadyToBoot and validate the final tables again.
5. Temporarily remove the Linux FDT configuration table, load Microsoft Boot
   Manager with boot policy enabled and start it under a five-minute UEFI
   watchdog. No BootOrder or temporary Boot#### entries are written.

The resident driver feeds the APSS watchdog during Boot Services and releases
that physical timer at ExitBootServices for Windows' CSRT HAL. Other operating
system handoffs keep their existing watchdog policy. EBS cleanup stops DMA
without freeing memory or invalidating the loader's memory-map key. No new
ExitBootServices wrapper is installed by this launcher. The resident errata
driver retains the upstream assembly capture of winload's original x19, x20
and return address, with its C implementation replaced in the private build
copy. The driver is enabled only by the Windows handoff policy.

The errata driver validates mapped ARM64 PE sections, unwind records, exports
and instruction context. It plans ACTLR_EL1 reads/writes, the specific cache
configuration bugcheck call and the PSCI MEM_PROTECT query before changing
either image. It translates kernel addresses under winload's application
context, restores the firmware context, validates writable physical aliases,
applies the complete plan and maintains instruction/data caches. The EBS path
does not allocate/free memory, scan a fixed physical window, change Windows
page-table permissions, or replace the Boot Services pointer during retries.
An unsupported loader layout returns an error before any instruction changes.

The context layout has been checked against ARM64 winload from Windows build
26200. Other layouts require a reviewed recognizer. This does not establish
that every Windows build or driver combination boots on Polaris. Unwind
parsing follows [Microsoft's ARM64 exception format](https://learn.microsoft.com/en-us/cpp/build/arm64-exception-handling),
including omitted leaf records and linked jump islands.

An application that returns before EBS restores the FDT and revokes the Windows
watchdog policy, allowing PJ4EDK2 to resume its devices. Stage/status text uses
large glyphs. This does not change Windows' own crash-screen font or GOP mode.
Best-effort diagnostics go to `EFI/Windows/boot-status.txt`, `acpi-before.aml`
and `acpi-prepared.aml`; log write failures do not block boot.
Returned handoffs also record the errata stage and instruction counts. Stage
2 finds the loader; 3 finds its context switch; 4 locates the kernel module;
5 plans kernel edits; 6 validates/applies physical edits; 7 is prepared and 8
is the original EBS result. A RAM child which stopped the complete USB backend
recreates its download/DMA buffers and timers before the menu resumes polling.

The platform contract follows [Renegade edk2-msm](https://github.com/edk2-porting/edk2-msm)
commit `e1952621f419f8db60ed28271264e1b5184c571d`, particularly
`PlatformBootManagerLib`, SDM845 `MsPlatformDevicesLib` and `KernelErrataPatcher`.
The build embeds that revision's checksum-valid CSRT with SHA256
`3906a2c857e09ba1dab9ab6f8de2b1f8e8ff7a012c8cb1f8da5e7a46ffdf9c72`.

The native tests exercise the real Polaris DSDT, bounded PE instruction plans,
the driver's context/physical-write lifecycle and EBS retry, and the launcher's
ordering and return recovery. They do not emulate SDM845 hardware or establish
that a particular Windows installation boots successfully.
