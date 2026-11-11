# VL fetch

A colorful `fastfetch`-style system information tool for Linux — written in C++ with a fish logo, truecolor gradients and pipeline-style info rows grouped into clean sections.

It has **no third-party dependencies** (just libc/libstdc++). Everything comes from `/etc/os-release`, `/proc`, `/sys` and small standard utilities, so it works on Arch, Debian/Ubuntu, Fedora, Void, Alpine, openSUSE, Gentoo, NixOS and friends.

```
                                o   user@host
                              o     ──────────────────────
                  ______            ▐ OS ───────────────────────────────────────────────────
               .-'      '-.             Distro │ Void Linux x86_64
             .'            '.           Kernel │ 6.18.59_1
            /    O      o    \            Boot │ Sat Oct 31 12:38
           |      o  o        |         Uptime │ 2 hours, 53 mins
       /\  |   o      o    o  >
      /  \ |     o  o      o  |     ▐ DEVICE COMPONENTS ────────────────────────────────────
      \  /  \   o      o     /            Host │ MS-7B89
       \/    '.            .'             BIOS │ 1.JC (08/17/2026)
               '-.______.-'                CPU │ AMD Ryzen 5 3600 (6C/12T) @ 4.21 GHz
         ~  V L   F E T C H  ~          Memory │ 5.99 GiB / 31.3 GiB (19%)  ██░░░░░░░░

                                    ▐ STORAGE ──────────────────────────────────────────────
                                      Disk (/) │ 41.5 GiB / 100 GiB (42%)  ████░░░░░░
                                          Swap │ 0 B / 1024 MiB (0%)  ░░░░░░░░░░

                                    ▐ NETWORK ──────────────────────────────────────────────
                                      Local IP │ 192.168.1.7
                                    Connection │ Ethernet
                                       Gateway │ 192.168.1.1
                                           DNS │ 192.168.1.1

                                    ▐ DESKTOP ──────────────────────────────────────────────
                                       Display │ 1920x1080 @ 60 Hz
                                       Session │ X11
                                            DE │ MATE
                                            WM │ Marco
                                      WM Theme │ BlackMATE
                                         Theme │ BlackMATE
                                         Icons │ mate
                                        Cursor │ Adwaita
                                          Font │ Inconsolata Bold Italic 10
                                    …
```

## Sections

| Section | Contents |
| --- | --- |
| **OS** | distro + architecture, kernel, boot time, uptime |
| **DEVICE COMPONENTS** | host model, BIOS, CPU (cores/threads/clock/temp), GPU(s), memory, battery |
| **STORAGE** | root filesystem usage, swap |
| **NETWORK** | local IP, connection type (Ethernet/Wi-Fi/VPN), gateway, DNS |
| **DESKTOP** | display resolution + refresh rate, session type, DE, WM, WM theme, GTK theme, icons, cursor, font |
| **SOFTWARE** | package counts (pacman, dpkg, rpm, apk, xbps, portage, flatpak, snap), shell + version, terminal + version, terminal font |
| **SYSTEM** | locale, timezone, init system, process count, load average, logged-in users |
| **COLORS** | 16-colour palette preview |

Empty/undetectable sections and rows are skipped automatically. Long values are shortened with `…` when the terminal is too narrow, and the fish is printed above the info when there is no room beside it.

## Build

```sh
make
# or
g++ -O2 -std=c++17 -Wall -Wextra -o vlfetch vlfetch.cpp
```

## Run

```sh
./vlfetch
```

Options:

| Option | Meaning |
| --- | --- |
| `-h`, `--help` | show help |
| `-v`, `--version` | print version |
| `--no-logo` | hide the fish |
| `--no-color` | disable ANSI colors |
| `--color` | force colors even when `NO_COLOR` is set |
| `--logo-only` | draw only the fish |

Colors are truecolor (24-bit). Set `NO_COLOR=1` to disable them globally.

## Install

```sh
sudo make install          # installs to /usr/local/bin/vlfetch
sudo make install PREFIX=/usr
```

## Notes

- The fish color palette changes with the detected distro (Void = green,
  Arch = blue/cyan, Ubuntu = orange, Fedora = blue, Debian = red, ...).
- Section headers and accents follow the same palette.
- Optional helpers are used only when present: `lspci`, `xrandr`,
  `gsettings`, `xfconf-query`, `glxinfo`, `flatpak`, `snap`, `who`.
- Tested on Void Linux (MATE, kitty).
- Requires C++17 and a Linux system with `/proc`.

## License

MIT — do whatever you want with it.
