# Target filesystem

Files that get installed **on the Pi**. The path under `target/` is the path on
the device, so `target/etc/systemd/system/pivid-tx.service` lands at
`/etc/systemd/system/pivid-tx.service`.

`scripts/` next door is the opposite: host-side tooling that never leaves the
build machine (`deploy.sh`, the two `build-*.sh` cross-builds, `embed_asset.sh`).

```
opt/pivid/
  pivid.conf                canonical config, the single source of truth for all
                            tunables. deploy.sh installs it ONLY if the device
                            has none, so field edits survive a re-deploy
  pivid-config-net.sh       config-mode network orchestration (AP up/down)
  pivid-wifi-client.sh      read/regenerate the WiFi client credentials
  pivid-display.sh          HDMI/composite output switching
  dnsmasq.conf.in           template; rendered to /run/pivid/dnsmasq.conf on
                            every config-mode entry

etc/systemd/system/
  pivid-tx.service          TX daemon (camera side)
  pivid-rx.service          RX daemon (receiver side)
  pivid-caster.service      LAN fan-out
  pivid-player.service      optional RX-local player on the HDMI output
  pivid-config-agent.service        always-on web config plane
  pivid-config-net.service          config-mode orchestrator
  pivid-config-hostapd.service      config-mode AP
  pivid-config-dnsmasq.service      config-mode captive DHCP/DNS

etc/modules-load.d/
  lr2021.conf               load the radio driver at boot
  bcm2835-codec.conf        load the VPU V4L2 M2M codec
  bcm2835-isp.conf          load the VC4 ISP

etc/modprobe.d/
  lr2021.conf               driver module options; the config agent REGENERATES
                            this file from pivid.conf on a modreload-tier apply

etc/tmpfiles.d/
  pivid-dma-heap-compat.conf   dma-heap compatibility symlink
```

Only the TX or RX unit is enabled on a given device — that role split is the sole
configuration difference between the two Pis.

Two of these are not static once deployed: `/etc/modprobe.d/lr2021.conf` is
rewritten by the config agent from `pivid.conf`, and `/run/pivid/dnsmasq.conf` is
rendered from `dnsmasq.conf.in` at config-mode entry. Editing the deployed copies
directly is pointless — they are overwritten.

See [../docs/DEPLOY.md](../docs/DEPLOY.md) for the install flow and the package
model.
