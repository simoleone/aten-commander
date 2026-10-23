# aten-commander

`atenswitch` is a tiny macOS command-line tool that switches an ATEN US3342 KVM to its other computer.

It replaces the switching part of ATEN's Bezel X app. There's nothing to keep running, no screen-recording or accessibility permissions, and no root. Bind it to a hotkey on each computer.

## Requirements

- macOS 12 or later, Apple Silicon or Intel
- A USB 3 link from each Mac to the KVM. The KVM's control device doesn't appear over USB 2.0, e.g. through a monitor hub set to USB 2.0.
- Bezel X not running. It holds the device exclusively.

## Install

```bash
make install
```

This installs to `~/bin/atenswitch`. Set `PREFIX` to install to `$PREFIX/bin` instead.

## Usage

```
atenswitch            switch to the other computer
atenswitch probe      check that the KVM's control device is reachable
atenswitch -n         print the command bytes without sending
```

## Hotkey

To bind it with the built-in Shortcuts app:

1. Shortcuts → Settings → Advanced → enable **Allow Running Scripts**.
2. Create a shortcut with a **Run Shell Script** action: `~/bin/atenswitch`.
3. Shortcut details (ⓘ) → **Add Keyboard Shortcut**.

Use the same combo on both Macs.

## How it works

The KVM contains a Genesys Logic control chip (USB `05E3:0627`). `atenswitch` sends it a single vendor SCSI command. Details are in [PROTOCOL.md](PROTOCOL.md).

Other devices built on the same chip may also work. Reports are welcome.

## Disclaimer

This project is not affiliated with or endorsed by ATEN or Genesys Logic. All trademarks belong to their owners.

It sends undocumented vendor commands to your hardware. Use it at your own risk.

## License

[MIT](LICENSE)
