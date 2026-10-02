# Building & Installing the Sluice `.deb`

This guide builds a Debian package for **Sluice** (the L4 TCP reverse proxy) and
installs it as a hardened systemd service. Run everything on a Debian/Ubuntu
host (or WSL/Ubuntu).

---

## 1. Prerequisites

```bash
sudo apt update
sudo apt install build-essential cmake dpkg-dev
```

- `build-essential` + `cmake` — compile the binaries.
- `dpkg-dev` — provides `dpkg-shlibdeps`, used to auto-compute the libc/libstdc++
  dependency for the package.
- **Network access** is required during `cmake`: the build fetches
  `nlohmann/json` and `cpp-httplib` via CMake `FetchContent`.

---

## 2. Build the binaries

```bash
cd <repo>/                 # the directory containing CMakeLists.txt
rm -rf build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
```

Release is the default, and it enables the hardening flags (stack canaries,
`_FORTIFY_SOURCE`, PIE/ASLR, full RELRO, non-exec stack) and **strips** the
binaries. This produces two executables in `build/`:

- `sluice` — the proxy daemon.
- `setup` — a manual bootstrapper (writes a default `/etc/sluice/config.json` if
  missing). Redundant once the package is installed, since the package ships the
  config as a conffile; kept for convenience.

---

## 3. Build the `.deb`

From the same `build/` directory:

```bash
cpack -G DEB
# or, equivalently:
make package
```

Output: `sluice_1.0.0_amd64.deb`.

> **Before publishing:** set a real maintainer in `CMakeLists.txt`
> (`CPACK_DEBIAN_PACKAGE_MAINTAINER`, currently a placeholder). Re-run `cmake ..`
> after editing.

Inspect the package before installing (optional):

```bash
dpkg-deb --info    sluice_1.0.0_amd64.deb    # control metadata + deps
dpkg-deb --contents sluice_1.0.0_amd64.deb   # file list
```

---

## 4. Install

Use `apt` (not raw `dpkg`) so dependencies resolve and the maintainer scripts
run:

```bash
sudo apt install ./sluice_1.0.0_amd64.deb
```

On install the package:

1. Creates a dedicated, unprivileged system user/group `sluice`.
2. Creates `/var/log/sluice` (owned by `sluice`) and `/etc/sluice`.
3. Installs the systemd unit and **enables + starts** `sluice.service`.

Check it:

```bash
systemctl status sluice
journalctl -u sluice -f          # live logs (startup summary goes to journald)
ss -tlnp | grep -E ':(8080|9090)'  # listener + dashboard (dashboard is loopback)
```

---

## 5. Configure & reload

Edit the config, then hot-reload with **zero downtime** (no restart, existing
connections preserved):

```bash
sudoedit /etc/sluice/config.json
sudo systemctl reload sluice     # sends SIGUSR1 -> apply_reload()
```

- `/etc/sluice/config.json` is a **dpkg conffile**: your edits survive package
  upgrades (dpkg will prompt on conflict).
- The dashboard defaults to **loopback** (`http://127.0.0.1:9090`). To expose it,
  set `dashboard.bind` and — importantly — `dashboard.token` in the config; the
  token is then required on config-save / reload API calls.
- Listen ports below 1024 work because the unit grants
  `CAP_NET_BIND_SERVICE` (the daemon still runs as the unprivileged `sluice` user,
  not root).

---

## 6. Logs & rotation

The shipped config logs to `/var/log/sluice/sluice.log` with the gateway's own
rotation set to `"none"` — **logrotate owns rotation**. The installed snippet
(`/etc/logrotate.d/sluice`) rotates daily and signals the daemon with `SIGHUP`,
which the log-writer process handles by reopening the file.

```bash
sudo logrotate --force /etc/logrotate.d/sluice   # test a rotation now
```

> Signal map: **SIGUSR1** = config reload · **SIGHUP** = log reopen (rotation) ·
> **SIGINT/SIGTERM** = graceful shutdown.

---

## 7. Upgrade

Rebuild with a bumped version (`project(sluice VERSION ...)` in `CMakeLists.txt`),
repackage, then:

```bash
sudo apt install ./sluice_<newversion>_amd64.deb
```

The service is restarted; your `/etc/sluice/config.json` is preserved.

---

## 8. Remove vs. purge

```bash
sudo apt remove sluice     # stops/disables the service, removes binaries;
                           # KEEPS /etc/sluice, /var/log/sluice, and the user
sudo apt purge  sluice     # also removes the sluice user, /etc/sluice, /var/log/sluice
```

---

## 9. What the package installs

| Path | Purpose |
|---|---|
| `/usr/bin/sluice` | proxy daemon |
| `/usr/bin/setup` | optional bootstrapper |
| `/etc/sluice/config.json` | default config (conffile) |
| `/lib/systemd/system/sluice.service` | hardened systemd unit |
| `/etc/logrotate.d/sluice` | log rotation policy |

---

## 10. Troubleshooting

- **`cpack: dpkg-shlibdeps not found`** → install `dpkg-dev`.
- **`cmake` fails downloading json/httplib** → you need network access, or vendor
  the two libraries and drop the `FetchContent` calls.
- **Service fails to start / permission denied writing logs** → confirm
  `/var/log/sluice` is owned by `sluice` (the postinst does this;
  `sudo chown -R sluice:sluice /var/log/sluice` to fix).
- **A runtime syscall gets blocked** (unlikely) → the unit sets
  `SystemCallFilter=@system-service`; loosen it in `sluice.service` and reload
  systemd (`sudo systemctl daemon-reload`).
- **Reload didn't apply** → check `journalctl -u sluice` for
  `hot-reload: done ...`; a parse error keeps the old config and logs
  `hot-reload FAILED`.

---

## 11. One-liner

```bash
cd <repo> && rm -rf build && mkdir build && cd build \
  && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j"$(nproc)" && cpack -G DEB \
  && sudo apt install ./sluice_1.0.0_amd64.deb
```
