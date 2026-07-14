# Radic

Radic is a lightweight Remote Desktop Protocol (RDP) client for Linux, built with Qt 6 and FreeRDP. It lets you connect to and control a Windows machine (or any RDP server) from a Linux desktop.

## Features

- Connect to RDP hosts with username/password and Windows domain authentication
- TLS certificate verification dialog, including a dedicated warning when a server's certificate has changed since it was last trusted
- Full screen mode and 1x/2x display scaling
- Optional dynamic resolution, so the remote desktop resizes to match the client window
- Mouse (click, move, wheel) and keyboard input forwarding, including a set of "magic key" shortcuts for controlling the client itself without them being intercepted by the remote session (see below)
- Bidirectional Unicode plain-text and bitmap image clipboard sharing with the remote session
- Per-connection settings (last used host, username, domain, window geometry) are remembered between sessions; passwords are never saved to disk

## Requirements

- Linux with an X11 or Wayland desktop session
- Qt 6 (Core, Gui, Widgets)
- FreeRDP 3.x (`libfreerdp3`, `libfreerdp-client3`, `libwinpr3`)

## Building

```bash
qmake6 Radic.pro
make
```

The resulting binary is `Radic`. If your system has both Qt 5 and Qt 6 installed, make sure to use the Qt 6 `qmake` (commonly `qmake6`) — using the wrong one will produce a broken build.

## Usage

Start the application and use **File → Connect** (or `Ctrl+Shift+Alt+N`) to open the connection dialog. Enter the host, username, password, and domain, then confirm to connect.

### Keyboard shortcuts

All of the shortcuts below use `Ctrl+Shift+Alt` as a prefix so they don't collide with anything you might send to the remote machine:

| Shortcut | Action |
|---|---|
| `Ctrl+Shift+Alt+N` | Open the connection dialog |
| `Ctrl+Shift+Alt+F` | Toggle full screen |
| `Ctrl+Shift+Alt+D` | Toggle 1x/2x display scale |
| `Ctrl+Shift+Alt+Backspace` | Toggle an on-screen command panel (visible while in full screen) |
| `Ctrl+Shift+Alt+CapsLock` | Toggle Caps Lock on the remote machine |
| `Ctrl+Shift+Alt+F4` | Exit full screen and close the window |
| `Ctrl+Shift+Alt+<any other key>` | Send `Ctrl+Alt+<key>` to the remote machine (e.g. to trigger Alt+Tab remotely) |

On keyboard layouts where the tilde (`~`) key is otherwise unused, it is remapped to act as a left Alt key, with a special key sequence to still type a literal tilde when needed.

### Menus

- **File → Connect / Disconnect** — open a new connection or close the current one
- **View → Dynamic Resolution** — resize the remote desktop to match the client window as you resize it

### Clipboard sharing

Plain text and bitmap images copied locally can be pasted into the remote session, and copied remote text or images can be pasted into local applications. Images use the RDP `CF_DIB` format and are limited to 64 MiB. Files, HTML formatting, alpha transparency, compressed DIB variants, and other rich formats are not transferred.

Due to RDP clipboard delayed rendering, Adobe Photoshop may not recognize the dimensions of a newly copied local image until the image has been pasted once. Caching the image, advertising `CF_DIB` first, and supplying explicit DPI metadata did not change this behavior, so it is currently treated as an interoperability limitation.

## Configuration

Settings (window geometry, last-used connection details) are stored at:

```
~/.config/soramimi.jp/Radic/Radic.ini
```

Passwords are never written to this file.

## Current limitations

- No audio redirection, file clipboard sharing, file transfer, or printer redirection yet
- Adobe Photoshop may recognize a redirected clipboard image's dimensions only after the first paste
- No persistent connection history/profile list (only the most recent connection is remembered)
- Linux only

## License

No license has been declared for this project yet.
