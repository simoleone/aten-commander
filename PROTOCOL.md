# US3342 control protocol

Reverse-engineered from ATEN's Bezel X 1.0.002 for macOS, to interoperate with hardware I own.
No ATEN code is included in this repository.

## Device

| | |
|---|---|
| USB ID | `05E3:0627` (Genesys Logic host bridge) |
| Presents as | SCSI peripheral type 05, "Virtual CD-ROM DEVICE", no media |
| Location | Behind the KVM's USB 3.2 hub (`0557:2415`), so it's SuperSpeed only |
| Visibility | Visible to both hosts, active and inactive |

On macOS, open the device with `kIOMMCDeviceUserClientTypeID`, then call `GetSCSITaskDeviceInterface` and `ObtainExclusiveAccess`. The plain SCSITask plug-in returns `kIOReturnUnsupported`.

## Identification

Send INQUIRY `12 00 00 55 48 00`, reading 72 bytes. The response contains `GENE` at offset 0x28.

## Command format

All commands are 16-byte CDBs:

| Byte | Meaning |
|---|---|
| 0 | `F2` (vendor opcode) |
| 1 | checksum, chosen so all 16 bytes sum to 0 mod 256 |
| 2 | sub-command |
| 3+ | arguments, zero-padded |

## Sub-commands

| Sub-command | Function | CDB | Data | Status |
|---|---|---|---|---|
| `07` | switch host | `F2 87 07 80 00…` | none | Works. `80` toggles. `00`, `01`, `81` and `82` are accepted but do nothing. |
| `08` | set KVM hotkey | `F2 FB 08 00 00 00 00 0B 00…` | 11 bytes out: `[modifier, 0, keycode, 0…]` | Accepted but has no effect. Bezel X never sends it. |

Bezel X's `08` menu offers these modifiers: L-Ctrl `01`, L-Shift `02`, L-Alt `04`, R-Ctrl `10`, R-Shift `20`, R-Alt `40`.

Bezel X's framework also runs a packet protocol over `F2` for its clipboard, file transfer and mouse-crossing features. Switching doesn't need it, and it isn't documented here.
