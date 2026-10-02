# 10. Production deployment

Sluice ships as a Debian package that installs it as a hardened systemd service —
dedicated unprivileged user, sandboxed, auto‑restart, and zero‑downtime reload via
`systemctl`. (Run this on the real Ubuntu host.)

### Build & install the package
```bash
cd build && cpack -G DEB
sudo apt install ./sluice_1.0.0_amd64.deb
```

### It's now a managed service
```bash
systemctl status sluice        # active (running), enabled on boot
systemctl reload sluice        # zero-downtime config reload (SIGUSR1)
```

### Show it's actually hardened
```bash
# runs as the unprivileged 'sluice' user, not root:
ps -o user= -p "$(systemctl show -p MainPID --value sluice)"

# systemd sandbox exposure score (lower = safer):
systemd-analyze security sluice | tail -n 1
```

### Expect
The service is running as user `sluice`, auto‑starts on boot, reloads config
without dropping connections, and reports a low (good) security‑exposure score.
The package also bundles the config as a preserved conffile and a logrotate rule.

*(Full details: `PACKAGING.md` at the repo root.)*

← [previous](09-scale.md) · [back to index](00-index.md)
