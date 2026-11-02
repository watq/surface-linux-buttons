# Surface Linux physical button toolbox

[简体中文](../../README.md) | **English**

Use a Surface Pro 8 without an attached keyboard: select a boot entry with the volume buttons, confirm with the power button, and lock/blank the display inside Linux while background tasks and networking keep running. The two modules work independently.

This is an independently maintained companion to [linux-surface](https://github.com/linux-surface/linux-surface). Follow that project's documentation for the kernel and device support. These tools have not been merged upstream.

## Status — 2026-11-02

| Module | Tested on Surface Pro 8 | Limits |
| :-- | :-- | :-- |
| Linux power button | Lock before blanking, wake to the lock screen, network and SSH remain available | GNOME Wayland, dedicated MSHW0040 power device, Intel backlight |
| GRUB buttons and original firmware OSK | Volume, power, back, original OSK Enter/Esc passed; no perceived hitch in the latest run | Logged sampling/redraw intervals remain; other boot paths are not universally certified |
| GTK toolbox | Dark GUI, version checks, enable/disable, rollback, logs | Local EFI build/signing required on another device |

The published GRUB source now matches the **2026-11-02 a844 D1** revision. GM18 binds that original Surface Pro 8 image and verifies physical buttons, original firmware OSK Enter/Esc, normal Ubuntu handoff and the owner’s no-perceived-hitch report. A locally rebuilt image is a new artifact and requires its own hardware acceptance.

## Features

- Independent `power-button/` and `grub-buttons/` modules.
- Lock confirmation before blanking: a failed or unconfirmed lock leaves the display on.
- Networking remains available because the operating system stays awake; this is display blanking, not suspend.
- Volume up/down selects an entry; press and release power to confirm; hold both volume buttons for about half a second, or press Esc, to return.
- The independent GRUB image imports the current system menu on every boot, keeping current kernel paths and system entries.
- A centered menu block with left-aligned text, large fonts, and footer instructions.
- Preserved original EFI, versioned user-owned images, pointer backups, and a reversible Linux service installer.

## Compatibility

| Device/environment | Linux display service | GRUB buttons |
| :-- | :-- | :-- |
| Surface Pro 8, Ubuntu 24.04, GNOME Wayland | Tested | Tested |
| Other Surface models | Unverified; event device and backlight need checking | Unverified; ACPI/GPIO contract must match |
| ROG Flow Z13 2023 / RTX 4060 and other convertibles | Unverified | The Surface GPIO implementation cannot be copied directly |
| KDE, XFCE, or X11 | Current service is not supported | Independent of the Linux desktop |

Matching Linux key codes or a similar tablet form factor does not establish firmware/GPIO compatibility.

## Linux power button

The service grabs only the dedicated MSHW0040 device whose sole key capability is `KEY_POWER=116`. It opens limited device descriptors as root and then drops to the selected desktop user. It uses persistent user D-Bus connections and system logind, confirms `LockedHint`, blanks the backlight, and sets Mutter `PowerSaveMode=3`. Waking restores the display and emits Shift to wake the screen shield without entering text or unlocking it.

Python's standard library and PyGObject are sufficient; `python3-evdev` is not required.

On an already logged-in GNOME desktop, replace `desktop` with the real non-root account:

```bash
sudo apt install python3-gi
python3 power-button/install.py --user desktop
sudo python3 power-button/install.py --user desktop --apply
systemctl status surface-screen-key.service
journalctl -u surface-screen-key.service -n 50
```

Without `--apply`, the installer only prints the plan. Applying saves existing files, service state, and GNOME settings before writing the service. It sets GNOME power-button action and AC/battery idle suspend to `nothing`, and logind power/idle actions to `ignore`. Other lid and low-battery policies remain independent.

Restore using the printed receipt:

```bash
sudo python3 power-button/install.py --restore /var/lib/surface-screen-key/backups/TIMESTAMP/receipt.json --apply
```

Restoration checks installed and saved hashes first, refuses to overwrite subsequent administrator edits, restores desktop settings and previous service state, and keeps the backup. `ExecStopPost` restores saved backlight brightness if the daemon stops. A hardware forced power-off is not modified.

See [the Linux module guide](../../power-button/README.md) for prerequisites, physical checks, and recovery details. Display blanking consumes more power than suspend because the system continues running.

## GRUB volume and power buttons

Linux key codes `114/115/116` come from the kernel input subsystem. They do not create events in the firmware's EFI keyboard interface. On the tested Surface, the extension validates the reviewed ACPI/Tiger Lake GPIO resources and reads button states directly without reconfiguring GPIO registers. A failed resource or ownership check disables sampling and leaves normal keyboard input available.

Build on **x86_64 Ubuntu Linux**:

```bash
sudo apt install build-essential autoconf automake bison flex gettext autopoint gawk python3 pkg-config grub-common sbsigntool
python3 grub-buttons/prepare-source.py build/source
cd build/source/grub-2.12
env AUTOPOINT=true ./autogen.sh
cd ../../..
mkdir -p build/grub
cd build/grub
env AWK=gawk ../source/grub-2.12/configure --with-platform=efi --target=x86_64 --disable-werror --disable-grub-mkfont --disable-device-mapper --disable-efiemu --disable-nls --disable-grub-mount --disable-libzfs
make -j2
cd ../..
```

The repository includes the pinned GRUB 2.12 archive, Ubuntu `2.12-1ubuntu7.3` patches, Surface core patch, source overlay, and themed resources. Preparation and compilation do not install a bootloader.

This recipe assumes `/boot/grub/grub.cfg` is on the root filesystem. A separate `/boot` layout requires adapting and verifying paths. Get the actual filesystem UUID, then build with a local signing key and certificate:

```bash
findmnt -no UUID -T /boot/grub/grub.cfg
python3 grub-buttons/build.py --grub-build build/grub --root-uuid ACTUAL_UUID --output dist/local-d1 --key /local/signing/owner.key --cert /local/signing/owner.pem
```

Secure Boot requires the certificate to be trusted by the target boot chain. See [linux-surface Installation and Setup](https://github.com/linux-surface/linux-surface/wiki/Installation-and-Setup). The scripts do not enroll a MOK, disable Secure Boot, or change BootOrder. Omitting both signing options creates an unsigned development image, which cannot be used under an enforcing Secure Boot chain.

Install the GUI as the desktop user:

```bash
sudo apt install python3-gi gir1.2-gtk-3.0
python3 grub-buttons/install-toolbox.py --release dist/local-d1
python3 ~/.local/share/surface-grub-keys/gui.py
```

The daily pointer starts disabled. Existing user installations are never overwritten. Use a separate ASCII `--base` directory when examining another build.

First test the new EFI manually from the original GRUB command line using the real UUID and path:

```text
search --no-floppy --fs-uuid --set=root ACTUAL_UUID
chainloader /home/USER/.local/share/surface-grub-keys/releases/VERSION/grub-daily.efi
boot
```

Verify buttons, normal keyboard input, system boot, and return. After acceptance, explicitly add the extension hook and enable the daily pointer:

```bash
sudo python3 grub-buttons/install-toolbox.py --base /home/USER/.local/share/surface-grub-keys --install-hook
python3 ~/.local/share/surface-grub-keys/tool.py daily-enable
```

The hook installer requires the original config to source `custom.cfg`, and refuses to overwrite an existing `custom.cfg`. Preserve and integrate an existing extension manually, with a backup and `grub-script-check`. The original EFI and generated menu are retained.

Disable or roll back through the GUI or CLI:

```bash
python3 ~/.local/share/surface-grub-keys/tool.py daily-disable
python3 ~/.local/share/surface-grub-keys/tool.py daily-rollback
```

Pointer changes preserve previous files. Without a previous release, rollback disables the entry. EFI boot logs use new `SP8-KEYDIAG/GMxxxx.LOG` files; the toolbox collects copies and keeps originals.

See [the GRUB module guide](../../grub-buttons/README.md) for detailed installation and recovery.

## Known issues and validation boundaries

- The owner reported no perceived hitch in the latest accepted run. D1 entry to first frame was 614ms and to resumed sampling 637ms; a later 1.06s sampling interval and slower redraws with the original OSK visible remain in the log. This is not a guarantee of zero waiting on every cold or Windows-to-GRUB restart.
- Other devices and desktops have not been validated. The power service and GRUB GPIO contract are hardware-specific.
- Historical physical Linux tests used `7.2.12-1-surface`; the device later updated to `7.2.13-1-surface`. Old results are not relabeled as independent new-kernel tests.
- Maintenance-signal tests and hardware power-button tests are recorded separately.
- User-owned EFI avoids direct package replacement, but kernel, firmware, shim/SBAT and GRUB updates can still change compatibility.
- The author's removal of memtest entries and an old kernel was a device-specific choice. Public scripts preserve the target machine's current entries and do not remove kernels or packages.

Run offline checks:

```bash
python3 tools/check.py
```

Checks cover Python syntax, the pinned archive, backup/restore, protection against later edits, release integrity, and path traversal. CI also checks all source patches. Offline simulation does not replace physical boot acceptance. See [validation records](../validation.md) and [metadata](../validation.json).

## Upstream and related projects

- [linux-surface/linux-surface](https://github.com/linux-surface/linux-surface): kernel, installation, and hardware compatibility.
- [GNU GRUB](https://www.gnu.org/software/grub/) and [Ubuntu grub2 source](https://launchpad.net/ubuntu/+source/grub2).
- [vinceliuice/grub2-themes](https://github.com/vinceliuice/grub2-themes): Vimix theme.
- [Microsoft Mu](https://github.com/microsoft/mu_plus): on-screen keyboard interface reference.
- [The owner's K90 Linux desktop repository](https://github.com/watq/gpu-desktop-for-android-container): publication/documentation format.

## License

Project code is GPL-3.0-or-later, with original GRUB and Ubuntu copyright notices retained. Theme and font resources retain their GPL and SIL OFL terms. See [licenses](../../licenses/README.md).

