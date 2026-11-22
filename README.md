# SiliconNT drivers

Windows drivers for Apple silicon Macs, part of SiliconNT: booting unmodified ARM64
Windows on those machines. Windows finds and installs these drivers the normal way,
from their INF files. The firmware describes the Apple devices in ACPI; the design is in
the SiliconNT repository (`docs/DESIGN.md`).

| Directory | Device | State |
|---|---|---|
| `ans/` | Internal disk controller, `ACPI\SLNT0001` | Skeleton: a Storport miniport that loads at boot, starts the device and reports no disks. It never maps or touches the controller's registers, never starts its coprocessor and does not connect its interrupts. |

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

## License

MIT, see `LICENSE`.
