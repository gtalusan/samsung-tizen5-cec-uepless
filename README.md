# samsung-tizen5-cec-uepless

Passive **HDMI input-name dumper** for rooted Samsung Tizen 5.x TVs — the
successor to [samsung-tizen5-cec](https://github.com/gtalusan/samsung-tizen5-cec)
that runs as a plain root binary instead of a signed TPK service.

`cecdump` reads `/dev/micom-cec` — the TV's raw CEC passthrough to the micom
(mode 0666, SMACK `*`) — learns the real names of the devices on each HDMI
port from the TV's own discovery dialogue, and serves them as JSON on
`http://<tv>:8787/` for a custom launcher such as
[CleanFrame](https://github.com/gtalusan/CleanFrame):

```json
{"hdmi1":null,"hdmi2":null,"hdmi3":"PlayStation 4","hdmi4":null,
 "devices":{"a4":{"name":"Apple TV","port":3},"a8":{"name":"PlayStation 4","port":3}},
 "cec":["845f823000", …]}
```

Port labels prefer the CEC name of the shallowest device on the port
(physical-address depth), so an AVR in front of a console labels the port
after the AVR. Ports with no learned name stay `null` and the launcher falls
back to "HDMI 1–4".

Verified on the 2020 Q90T (`T-NKMAKUC-REL-202505100505`, Tizen 5.5, armv7,
kernel 4.1.10), rooted via
[samsung-tv-usb-root](https://github.com/gtalusan/samsung-tv-usb-root).

## How names are obtained

There is **no public API** for input-source names on this platform; the CEC
bus is the only name source (see the predecessor's README for the dead ends).

The micom delivers CEC frames prefixed with a length byte (`0x80|len`). The
dumper listens passively and decodes:

- `<Set OSD Name>` (`0x47`) — device name by initiator address
- `<Report Physical Address>` (`0x84`) — maps a device to its HDMI port (PA
  top nibble) and its depth behind a receiver

Names arrive when **the TV itself asks**: during source switches, hotplugs,
device power-ons and the boot-time discovery dialogue. The micom **accepts
writes but never transmits foreign frames** — sending `<Give OSD Name>`
yourself does nothing. Passive-only is the design.

## Build and test (Docker)

```sh
docker build -t cecdump-builder .
docker run --rm -v "$PWD:/src" -w /src cecdump-builder ./build.sh     # -> ./cecdump
docker run --rm -v "$PWD:/src" -w /src cecdump-builder ./test/test.sh  # end-to-end via a FIFO
```

`cecdump` is a single static armv7-musl binary (musl.cc cross toolchain,
soft-float, pinned by sha256 in the Dockerfile). No nix, no Tizen SDK.

## Deploying to the TV

```sh
ssh root@tv 'cat > /opt/bin/cecdump && chmod 755 /opt/bin/cecdump' < cecdump
mkdir -p /opt/usr/share/tvroot/cec   # state file directory
```

The boot hook (`/opt/usr/share/tvroot/boot.sh`, see the Q90T root fork) starts
it with the same setsid-detached retry loop as dropbear — both binaries are
unsigned, so both must wait out the UEP gate patch window on stick boots:

```sh
setsid /bin/sh -c '
i=0
while [ $i -lt 36 ]; do
  grep -q ":224F .* 0A " /proc/net/tcp && exit 0
  [ -x /opt/bin/cecdump ] || exit 0
  ( /opt/bin/cecdump )
  i=$((i+1)); sleep 5
done
' </dev/null >>/opt/usr/share/tvroot/cec/cecdump.log 2>&1 &
```

Run standalone: `setsid /opt/bin/cecdump </dev/null >>…/cecdump.log 2>&1 &`

Env overrides: `CECD_DEV`, `CECD_STATE`, `CECD_PORT` (used by the container
test; defaults are the TV paths).

## Field notes (read before touching)

- **Replacing the binary on a live TV turns the TV OFF.** Killing the running
  cecdump strands its reader thread as an unreapable zombie (see below) and
  Samsung's health watchdog detects the stuck process and powers the TV down.
  Deploy updates the safe way: `cat > /opt/bin/cecdump.new && mv -f
  cecdump.new cecdump`, then **reboot the TV** — the boot loop starts the
  fresh binary cleanly, and the state file carries the learned names across.
- **The micom read is uninterruptible.** The reader thread parks in a driver
  wait that ignores `O_NONBLOCK` and never returns. A process that exits with
  the reader blocked strands itself as an unreapable zombie (`Z` + threads)
  and **pins its binary** (`Text file busy` for direct overwrites). Therefore
  `cecdump` binds and listens **before** opening the device — a failed bind
  exits cleanly with no reader.
- **UEP**: unsigned binary — it execs only while the UEP gate is patched down
  (any boot whose kernel stage ran: warm reboots with the stick DO fire the
  trigger on this TV, verified 2026-10-25). On stickless boots the retry loop
  gives up quietly after ~3 minutes and CleanFrame keeps plain labels.
- `SIGTERM` closes the listener before exiting so the port always frees; the
  boot loop then restarts it on its own.
- State file: `/opt/usr/share/tvroot/cec/cec-names.txt`, same line format as
  the old TPK service (`N<addr>|<name>`, `P<addr>|<port>|<depth>|<n1>|<n2>|<n3>`,
  `A|<port>|<n1>|<n2>|<n3>` for the active stream PA) so the two
  implementations stay interchangeable.

## License

MIT — see LICENSE.
