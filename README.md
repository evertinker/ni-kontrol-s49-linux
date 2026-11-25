# Kontrol S49 MK3 instrument browsing on Linux

Make the **Native Instruments Kontrol S49 MK3's built-in instrument browser** work with Kontakt 8 on Linux—both standalone and inside Ableton Live 12 running through [shibco/ableton-linux](https://github.com/shibco/ableton-linux).

**This project extends [NI-Wine](https://github.com/selimbucher/native-instruments).** NI-Wine installs and runs Native Access, Kontakt 8, and other Native Instruments software in a Wine environment. This repository addresses the additional USB device-discovery and socket-communication issues that prevent the S49 MK3's hardware instrument browser from working with that software, including Kontakt hosted inside Wine-based Ableton Live.

## The problem

The S49 MK3 can send MIDI notes to Linux applications, and its DAW-control MIDI port can operate Ableton's transport and mixer. But **MIDI working does not mean the keyboard's instrument browser works**.

Browsing and selecting Kontakt instruments from the keyboard's own screen requires a separate connection to Native Instruments' **Hardware Connection Service**. On Windows, the NI software can discover the keyboard and communicate with it through a dedicated USB interface. Under Wine, parts of that Windows USB access and device-discovery path are missing. So Kontakt itself may run and play normally while the keyboard's browser cannot connect.

Running Kontakt inside Wine-based Ableton introduces another obstacle: Ableton uses a **different Wine prefix** from the NI installation, and the customized Wine build used by shibco's project cannot open the Unix-domain sockets needed to reach the NI hardware service. Without an additional bridge, the instrument browser still cannot talk to Kontakt inside Live.

In short: **the keys can work, the plugin can work, and DAW transport can work—while hardware instrument browsing remains broken.** This project connects those missing pieces.

## What this project provides

- **Keyboard USB access:** A Linux USB helper and replacement Windows components let the NI Hardware Connection Service discover and communicate with the S49 MK3.
- **Standalone Kontakt support:** Restores the service connection for Kontakt 8 running in the Native Instruments Wine prefix.
- **Wine-based Ableton support:** A local socket bridge and DLL hook connect Kontakt running in Ableton's separate Wine prefix to the NI hardware service.

The hardware-browser connection is **separate from ordinary MIDI and Ableton control-surface setup**. This project does not replace the Komplete Kontrol S Mk3 DAW-control profile.

**Tested environment:** CachyOS, Kontrol S49 MK3, Kontakt 8 standalone, and Kontakt inside [shibco/ableton-linux](https://github.com/shibco/ableton-linux) (Wine 11.13). This is an unofficial community project, not affiliated with Native Instruments or Ableton. It is **not a yabridge patch**; hosting NI plugins in native Linux DAWs through yabridge is a separate setup.

## How it works

The keyboard's instrument browser talks to `NIHardwareConnectionService.exe` over a USB bulk interface (interface 3 on USB `17cc:2100`). Kontakt then talks to that service over a Unix socket inside the Wine prefix.

Wine's `winusb.dll` and `pnputil.exe` are stubs, and Wine does not publish the device interface the service scans (`{9CB5BE68-52CA-4F04-A394-090391A15E51}`). The pieces in this repo fill those three gaps:

- `ni-s49-usb` claims interface 3 and speaks a small local protocol on `127.0.0.1:47653`.
- `winusb.dll` and `pnputil.exe` are stand-ins the service loads from the NI prefix.
- `s49-iface.exe` registers that device interface.

The Wine build used by ableton-linux cannot open `AF_UNIX` sockets at all. `ni-nks-bridge` listens on `127.0.0.1:47654` and connects to the real agent socket. `ni-afunix-hook.dll` turns `AF_UNIX` connects in the Ableton process into that bridge. Ableton's `LoadAppInitDlls` is empty, so the hook is loaded from a stand-in `version.dll` next to `Ableton Live 12 Suite.exe`.

## Build

**Prerequisite:** Set up [NI-Wine](https://github.com/selimbucher/native-instruments) first, including Native Access and Kontakt 8. This project does not install Native Instruments software or replace NI-Wine.

Build dependencies: `gcc`, `libusb-1.0`, `x86_64-w64-mingw32-gcc`.

```sh
make
make install
```

`make install` copies the Linux programs to `~/.local/bin`, `s49-iface.exe` to `~/.local/lib/ni-s49`, and the user services to `~/.config/systemd/user`. It does not copy anything into a Wine prefix.

## Native Instruments prefix

Use the **NI-Wine prefix** where Native Access and Kontakt are installed. NI-Wine's default is `~/.wine-ni`; if you configured a custom location (for example through `NI_WINE_PREFIX`), use that instead. The commands below use `NI_WINE_PREFIX` when set, otherwise `~/.wine-ni`. Use the Wine executable for your NI-Wine setup, **not** the Ableton Wine runtime.

Back up the two files before replacing them:

```sh
P="${NI_WINE_PREFIX:-$HOME/.wine-ni}"
SYS="$P/drive_c/windows/system32"
cp -n "$SYS/winusb.dll" "$SYS/winusb.dll.wine-builtin"
cp -n "$SYS/pnputil.exe" "$SYS/pnputil.exe.wine-builtin"
cp build/winusb.dll "$SYS/winusb.dll"
cp build/pnputil.exe "$SYS/pnputil.exe"
cp build/winusb.dll \
  "$P/drive_c/Program Files/Common Files/Native Instruments/Hardware/Hardware Connection Service/winusb.dll"
```

Register the overrides with that same wine:

```sh
WINE="${NI_WINE:-$HOME/.local/bin/wine}"
WINEPREFIX="$P" "$WINE" reg add 'HKCU\Software\Wine\DllOverrides' \
  /v pnputil.exe /t REG_SZ /d native /f
WINEPREFIX="$P" "$WINE" reg add \
  'HKCU\Software\Wine\AppDefaults\NIHardwareConnectionService.exe\DllOverrides' \
  /v winusb.dll /t REG_SZ /d native /f
```

The agent socket path has to fit in a Unix `sun_path` (108 bytes). If the prefix path is too long, the service never listens. A shorter `drive_c` path, or a symlink, fixes that. Set `NI_AGENT_DIR` to the directory that actually contains `com.native-instruments.odr_agent.kks`.

## USB permission

`udev/70-kontrol-s49-mk3.rules` tags the keyboard so the logged-in user can open it. Install it as root, then replug:

```sh
sudo cp udev/70-kontrol-s49-mk3.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=17cc
```

## Wine Ableton

Only needed for browse inside the Wine Live from ableton-linux. Standalone Kontakt does not need the bridge.

Copy Wine's own `version.dll` out under another name, then install the stand-in beside Live. With the shibco runtime this wine is `~/.local/opt/wine-d2d1-nspa-11.13`.

```sh
A="$HOME/.wine-ableton"
cp "$HOME/.local/opt/wine-d2d1-nspa-11.13/lib/wine/x86_64-windows/version.dll" \
  "$A/drive_c/ni-version-real.dll"
cp build/ni-afunix-hook.dll "$A/drive_c/ni-afunix-hook.dll"
cp build/version.dll \
  "$A/drive_c/ProgramData/Ableton/Live 12 Suite/Program/version.dll"
```

Using the Ableton wine binary, not the NI one:

```sh
AW="$HOME/.local/opt/wine-d2d1-nspa-11.13/bin/wine"
WINEPREFIX="$A" "$AW" reg add \
  'HKCU\Software\Wine\AppDefaults\Ableton Live 12 Suite.exe\DllOverrides' \
  /v version.dll /t REG_SZ /d 'native,builtin' /f
```

Do not point the Ableton prefix at wine-staging, and do not install Native Access into it. The hardware service stays in the NI prefix. The bridge is what joins them.

In Live's Link, Tempo & MIDI preferences, set one control surface to Komplete Kontrol S Mk3. On this Wine the DAW port is `MIDIIN2 (KONTROL S49 MK3)` and `MIDIOUT2 (KONTROL S49 MK3)`.

## Run it

```sh
systemctl --user daemon-reload
systemctl --user enable --now \
  ni-s49-usb.service ni-s49-hcs.service ni-nks-bridge.service ni-s49-plug.service
```

Edit the `Environment=` lines in `ni-nks-bridge.service` and `ni-s49-hcs.service` if your prefix is not `~/.wine-ni`.

Plug the keyboard in, wait a few seconds, then start Kontakt or Ableton. A saved Live set that already contains Kontakt connects when the set loads, as long as the services are up before Live starts.

`ni-s49-plug` restarts the USB helper and the hardware service when the keyboard is plugged in. Live does not resend the Ableton profile if the keyboard restarts while Live is open. Toggle the Komplete Kontrol S Mk3 control surface off and on. Kontakt keeps the old browse connection until that plugin is removed from the track and added again. Closing Live before unplugging the keyboard avoids both steps.

## Services

| Unit | Role |
| --- | --- |
| `ni-s49-usb.service` | USB interface 3 |
| `ni-s49-hcs.service` | device interface, then NIHardwareConnectionService |
| `ni-nks-bridge.service` | Ableton's missing `AF_UNIX` |
| `ni-s49-plug.service` | restart browse after a replug |

Logs: `/tmp/ni-s49-usb.log`, `/tmp/ni-s49-iface.log`, `/tmp/ni-nks-bridge.log`, and `NIHardwareConnectionService.log` in the prefix temp directory. A good hardware-service log ends with `device connected: Kontrol S49 MK3`.

If the Ableton template loads but the plugin page says no parameters are mapped, check those two logs. `CreateFile` on `{9CB5BE68-52CA-4F04-A394-090391A15E51}` must succeed. Error 2 there means the DOS device was created for the generic USB guid instead, and the hardware log says the bulk pipes could not be connected. Restarting the keyboard or Live runs that same publisher again. Rebuild, `make install`, and restart `ni-s49-hcs.service`.
