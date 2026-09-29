# unifyctl

A small C11 command-line tool for managing stored mouse and keyboard pairings on classic Logitech Unifying USB receivers. It uses Linux hidraw and libudev, or IOKit HID on macOS, without detaching or seizing the operating system's keyboard and mouse drivers.

The initial allowlist is `046d:c52b` and `046d:c532`, management interface 2, with HID++ report descriptor validation. Bolt, Lightspeed, Nano, Bluetooth, battery reporting, and peripheral settings are outside this version's scope.

**Validation status:** the native macOS IOKit backend builds without warnings on macOS 27.0 (arm64). `make test` and `make sanitize` (ASan/UBSan) passed on September 29, 2026, including invalid macOS receiver-ID checks, portable protocol/operation tests, report queue tests, and signal handling tests. These checks do not exercise a physical receiver. Linux build/udev validation and physical receiver validation remain outstanding in this workspace; see the [hardware checklist](docs/hardware-validation.md) for manual checks.

## Build and install

### Linux

On Debian/Ubuntu:

```sh
sudo apt-get install build-essential pkg-config libudev-dev python3
make
make test
make sanitize
sudo make install
```

On Fedora, the corresponding development dependencies are `gcc`, `make`, `pkgconf-pkg-config`, `systemd-devel`, and `python3`.

### macOS

Install the Xcode Command Line Tools if needed:

```sh
xcode-select --install
```

Then build and run the automated checks:

```sh
make
make test
make sanitize
sudo make install
```

The build selects the native IOKit backend automatically and links the system `IOKit` and `CoreFoundation` frameworks. No third-party C libraries are required. `make test` and `make sanitize` also require `python3` on `PATH`. Installation under `/usr/local/bin` may require `sudo`; run `unifyctl` itself as your normal user.

### Build targets and installation

The executable is `build/unifyctl`. `make install` installs only the binary under `/usr/local/bin`; override `PREFIX` and `DESTDIR` as needed. It does not install permission rules automatically. Python is used only by the CLI tests. Runtime dependencies are libc and libudev on Linux, and system frameworks on macOS.

On other platforms, `make test` builds the portable core with a backend that rejects hardware commands. This supports help, parsing, protocol, operation, and POSIX transport tests.

## Permissions

### Linux

Even `list` requires read/write access to the management hidraw node because read-only queries are sent as output reports. It does not write receiver registers or change stored state.

For a local desktop session with systemd-logind, install the narrowly scoped rule:

```sh
sudo install -m 644 packaging/70-unifyctl.rules /etc/udev/rules.d/70-unifyctl.rules
sudo udevadm control --reload-rules
```

Reconnect the receiver to apply the rule. It grants active-session access only to supported receiver management interfaces, not all HID devices. Do not permanently run this tool as root or use `chmod 666 /dev/hidraw*`.

For a headless system, an administrator can create a dedicated `unifyctl` group, add only authorized users, and replace `TAG+="uaccess"` in the final matching rule with `GROUP="unifyctl", MODE="0660"`. Keep the VID/PID and interface restrictions intact, reload rules, reconnect, and start a new login session.

### macOS

No rules file is needed and the tool must not be run with `sudo`. It opens only the receiver's vendor-defined HID++ interface, in shared (non-seizing) mode, so the system keeps handling the receiver's keyboard and mouse interfaces. macOS gates keyboard-class HID access behind **Privacy & Security → Input Monitoring**; the HID++ interface is vendor-defined and is not expected to require it, but this is unverified. If opening fails with an access error, check that setting for the terminal application and try again.

Another process can open the same interface. Quit Logitech Options/Options+, Logi Tune, Solaar, and similar managers before pairing or removing devices.

## Usage

```sh
unifyctl help
unifyctl --help
unifyctl list --help
unifyctl add --help
unifyctl remove --help

unifyctl list
unifyctl --receiver /dev/hidraw2 list                # Linux
unifyctl --receiver DevSrvsID:4294968397 list        # macOS
unifyctl --receiver /dev/hidraw2 --debug list

# These commands change receiver pairings. Run only when intended.
unifyctl add --timeout 30
unifyctl remove 2
unifyctl remove 2 --yes
```

Global options may appear before or after the command. Exactly one supported receiver is selected automatically; with several, the program lists candidates and requires `--receiver PATH`. On Linux, `PATH` is the management interface's hidraw node. On macOS, it is the interface's IORegistry entry ID in the `DevSrvsID:<number>` form also used by hidapi; the ID is stable until the receiver is unplugged. Run `unifyctl list` with several receivers attached to print the candidates. Explicit receivers are validated before any HID++ traffic. Help needs no attached receiver or device permissions.

`list` scans all six stored slots, including gaps. Sleeping and powered-off devices remain listed. An empty receiver prints `No paired devices.` and exits successfully. Output contains slot, stored name, type, WPID, optional serial number, and connection status. WPID identifies a model, not a unique device. Names are receiver-stored codenames, at most 14 bytes. Unsupported optional metadata is `unknown`; communication errors are not silently hidden.

Connection status is `unknown` in read-only listing: the tool does not enable or solicit notifications to obtain it. A lack of notifications is not evidence of disconnection.

`add` opens a pairing window for 1–255 seconds, default 30. Activate the peripheral's Unifying pairing mode when prompted. The tool waits for pairing notifications and verifies a new stored slot; accepting the open-window command is not success. Ctrl-C and SIGTERM attempt bounded cleanup. Individual requests have a 2-second timeout. Pairing verification has a shared 5-second budget and can retry each identical stored-record read once after a reply timeout. This accommodates traffic during device initialization while still checking the echoed record selector. Removal verification and cleanup each retain a 2-second budget. Pairing-state writes are never retried. On macOS, report writes use a synchronous IOKit call with its own transfer timeout, so an in-progress write can delay cancellation or exceed the application-level request and cleanup budgets.

`remove SLOT` accepts slots 1–6. It shows the stored device and requires `y` or `yes` followed by Enter. All other answers and EOF decline. Without `--yes`, both input and the confirmation output must be terminals. It rechecks the device after confirmation, sends one unpair command, and verifies the slot is empty. Removal disconnects the device from this receiver.

Do not run another receiver manager (including Solaar, ltunify, or Logitech Options+) concurrently with pairing operations. An advisory lock prevents overlapping `unifyctl` sessions, but other programs and kernel clients may not honor it. On macOS the lock file lives in the per-user temporary directory, so it coordinates only sessions of the same user. Protocol replies lack transaction sequence numbers, so external interference cannot always be detected.

## Errors and troubleshooting

Results go to stdout; prompts, errors, and `--debug` hexadecimal traffic go to stderr. Debug output can include peripheral names and serial numbers; review it before sharing.

| Exit | Meaning |
|---|---|
| 0 | Success, including help or an empty receiver |
| 1 | Internal/resource initialization failure |
| 2 | Invalid command or arguments |
| 3 | Missing, ambiguous, unsupported, or incorrectly selected receiver |
| 4 | Device access or advisory locking failure |
| 5 | Transport failure or receiver disconnection |
| 6 | Request or pairing timeout |
| 7 | Protocol/operation failure, unverified result, or incomplete cleanup after pairing |
| 8 | Confirmation declined or unavailable |
| 130 | Cancelled by SIGINT |
| 143 | Terminated by SIGTERM |

- **Permission denied (Linux):** inspect the selected node's permissions and udev properties; confirm the local session is active, then reconnect after installing the rule.
- **Access denied (macOS):** check Privacy & Security → Input Monitoring for the terminal application. "Held exclusively by another process" means another program seized the interface; quit Logitech software and retry.
- **Multiple receivers:** use one of the printed hidraw paths or `DevSrvsID:` values with `--receiver`.
- **Unsupported interface:** do not substitute an arbitrary Logitech hidraw node or IOKit device. Check the receiver family, VID/PID, and management interface. On macOS, `ioreg -r -c IOHIDDevice -l` shows `VendorID`, `ProductID`, and the parent `bInterfaceNumber`.
- **Timeout or protocol error:** stop competing receiver managers and collect a `--debug list` trace. Errors include the raw protocol code when available.
- **Uncertain pairing/removal outcome:** run `list` before retrying. Pairing-state writes are never automatically resent.
- **Receiver reports successful pairing, then verification fails:** the device may already be paired and working. Run `list` to confirm its stored slot. Linux can query a newly connected device even with no other receiver manager running; a response for a different record is ignored, and verification retries the same read once. Persistent interference still returns a nonzero status rather than claiming a verified inventory.
- **Cleanup warning:** the receiver may have changed state even when an acknowledgement was lost. Closure is attempted once; when the session is ambiguous, notification restoration may be skipped and is reported. Check stored state and reconnect the receiver if needed. A receiver-side pairing timeout also bounds the open window.

## Development

See [architecture](docs/architecture.md), [protocol provenance](docs/protocol.md), [validation checklist](docs/hardware-validation.md), and the original [implementation plan](docs/IMPLEMENTATION_PLAN.md).

`make test` never pairs or unpairs hardware. It uses synthetic protocol fixtures, an anonymized user-supplied pairing trace with synthetic timing and recovery replies, and socket pairs. Platform-specific CLI checks use deliberately invalid Linux receiver paths and macOS receiver IDs. Tests cover sparse stored slots, optional metadata, malformed reports, notification/reply matching, bounded read retries, deadlines, pairing outcomes, cancellation, confirmation, removal verification, signal-handler lifecycle, macOS receiver-ID parsing, and the callback report queue. The macOS CLI tests exercise receiver selection failures before HID traffic, and the portable tests cover the callback report queue. Actual IOKit report transfers, receiver disconnection, and shared access with normal keyboard/mouse input still require the manual hardware checklist.

Source and test code use 1TBS. A one-statement `if` or `else if` body stays on the condition's line without braces, except when braces avoid a dangling `else`. No automatic formatter is configured because common defaults would rewrite this style.

The implementation is BSD-3-Clause. Referenced GPL projects were consulted for protocol behavior; their implementation source was not copied.
