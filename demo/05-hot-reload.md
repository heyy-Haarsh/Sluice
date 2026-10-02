# 5. Zero‑downtime hot reload

The config can change while running — add/remove routes or backends — with no
restart and without dropping a single live connection.

### Run
```bash
# Start a 4th backend, then add it to the pool live:
python3 tests/backends/demo_backend.py D 8004 &
jq '.routes[0].backends += [{"host":"127.0.0.1","port":8004}]' config/demo.json > /tmp/d && mv /tmp/d config/demo.json
kill -USR1 $(pgrep -f 'sluice config/demo.json')

# D now serves traffic — no restart happened:
for i in $(seq 1 8); do curl -s http://127.0.0.1:9000/ | grep -o 'Backend [ABCD]'; done
```

### Expect
Sluice logs `hot-reload: done`, the process never stopped, and **D** now appears
in rotation alongside A/B/C. (Optional: the dashboard's config editor can make the
same change from the browser.)

← [previous](04-failover.md) · next → [IP access control](06-access-control.md)
