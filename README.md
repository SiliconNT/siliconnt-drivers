# SiliconNT drivers

Windows drivers for Apple silicon Macs, part of SiliconNT: booting unmodified ARM64
Windows on those machines. Windows finds and installs these drivers the normal way,
from their INF files. The firmware describes the Apple devices in ACPI; the design is in
the SiliconNT repository (`docs/DESIGN.md`).

| Directory | Device | State |
|---|---|---|
| `ans/` | Internal disk controller, `ACPI\SLNT0001` | Read-only Storport miniport: starts the controller's coprocessor, shows the internal disk to Windows as write protected, reads; refuses every command that writes and every pass-through request. Not yet run on a Mac. No crash dump or hibernation. |

`ans/` in more detail:

| Path | What |
|---|---|
| `ans/core/` | Everything that talks to the controller (coprocessor, mailbox, SART, NVMe queues, reset), from m1n1 v1.9.9's MIT files (`LICENSE.m1n1`). Plain C with no operating system in it: the same code runs in the m1n1 test guest and, later, the firmware. The only NVMe commands it can send are on a whitelist: create and delete the I/O queues, identify, read. `ans_core.h` and `ans_env.h` are its interface. |
| `ans/core/test/` | The model test: the core against a fake controller on the host. |
| `ans/miniport.c` | Storport's entry points. |
| `ans/env_storport.c` | The core's environment on Windows (registers, memory for the device, waiting, the log). |
| `ans/scsi.c` | What the disk answers to SCSI commands; refuses writes. |
| `ans/dsd.c` | Reads the ACPI node's `_DSD`. |
| `ans/log.c`, `format.c` | The driver's log: a ring in the driver's own memory, readable without a debugger (see below). |
| `ans/test/` | The host test of `scsi.c`, `dsd.c` and `format.c`. |

The drivers are test-signed. A machine runs them only with test signing turned on in its
boot configuration; the SiliconNT boot disk does that.

## What you need

- An x64 Windows machine. No administrator rights are needed.
- The Enterprise WDK ISO `EWDK_ge_release_svc_prod1_26100_250904-1728.iso`
  (20002537472 bytes, SHA-256
  `9F48251DD24AD31AAC206D8256E95BDA5F90A9783982C45A8AAFEB9054562379`),
  by default in your `Downloads` folder (`-IsoPath` to point elsewhere). The scripts check
  it, mount it, use its compiler and tools, and unmount it again. Nothing else is
  installed or used for building.

## Once per machine: a test certificate

```powershell
powershell -ExecutionPolicy Bypass -File .\new-test-cert.ps1
```

This writes `siliconnt-test.pfx` (certificate and private key, no password) and
`siliconnt-test.cer` (certificate only) to `%USERPROFILE%\SiliconNT-build\signing\`
(`-SigningDir` to choose another place). Keep that folder outside every repository and
never share the `.pfx`. Nothing is put into any certificate or key store.

## Build and sign

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

```powershell
powershell -ExecutionPolicy Bypass -File .\sign.ps1
```

`build.ps1` builds `Release|ARM64` with the EWDK's MSBuild and checks each INF with
`infverif /w`; `sign.ps1` signs each `.sys`, makes its catalog with `Inf2Cat`
(`/os:10_GE_ARM64`) and signs the catalog. Both print the SHA-256 of what they made.
`build.ps1` writes the commit into the driver (it shows in the driver's log), with
`_dirty` after it when the working tree has changes: build packages that go onto a Mac
from a clean, committed tree. Every build gives a `.sys` with a different SHA-256.

Output, all under `out\` (deleted and remade by every build):

| Path | What |
|---|---|
| `out\ans\` | The driver package: `ans.sys` (signature embedded), `ans.inf`, `ans.cat` (signed) |
| `out\siliconnt-test.cer` | The certificate the package is signed with |
| `out\build\` | Intermediate files, `ans.pdb`, the MSBuild log |

To look at the result, with the EWDK mounted (here as `G:`):

```powershell
& 'G:\Program Files\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe' verify /v /pa .\out\ans\ans.sys
```

The chain ends in the test certificate, which this machine does not trust, so `signtool`
reports an untrusted root. That is expected.

## Test

```powershell
powershell -ExecutionPolicy Bypass -File .\test.ps1
```

Runs, with the EWDK's compiler on the host (x64, AddressSanitizer): the model test of the
core (`out\test\model_test.exe`) and the host test of the miniport's SCSI layer, `_DSD`
parser and formatter (`out\test\host_test.exe`); then builds the core as ARM64 kernel
code. Each test program prints one line per test and a summary; the script stops at the
first failure and ends with `Model test and host test passed`.

## The `ans` log

`ans.sys` keeps its log in memory of its own, which it never gives back, so the log is
still there after Windows unloads a driver whose device did not start: 4 KiB pages that
each start with the 16 bytes `SiliconNTansLog1`, one page of counters and a ring of text
lines. The layout is at the top of `ans/log.h`. It is there to be read without a debugger,
from a copy of the machine's physical memory taken after the guest is stopped; with a
debugger it is `dx *ans!g_ans_log`, and each line also goes to the debugger. The log never
holds the disk's serial number or anything read from the disk.

## License

MIT, see `LICENSE`.
