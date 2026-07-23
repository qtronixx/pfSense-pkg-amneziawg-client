# pfSense-pkg-amneziawg-client
[Читать на Русском](README.ru.md) | **[English]**

A client-side **AmneziaWG** plugin (a WireGuard fork with protocol obfuscation for DPI evasion) for **pfSense 2.8.1 Community Edition**.

> ⚠️ **Client mode only.** This plugin connects pfSense as a client to an external AmneziaWG server. There is no functionality for running an AmneziaWG server in this package, and none is planned.

> ⚠️ **Status: working prototype, under active testing.** Core functionality (tunnel creation, obfuscation, real traffic through the VPN) has been confirmed by hands-on testing, including a full "clean-slate" install run. Known limitations and open tasks are listed in the [Roadmap](#roadmap--known-limitations) section below.

---

## Contents

- [Architecture](#architecture)
- [Requirements](#requirements)
- [Installation](#installation)
- [Update and removal](#update-and-removal)
- [Setting up your first tunnel](#setting-up-your-first-tunnel)
- [⚠️ Mandatory pfSense GUI steps](#️-mandatory-pfsense-gui-steps-traffic-wont-pass-without-them)
- [Importing an existing .conf](#importing-an-existing-conf)
- [Diagnostics and logs](#diagnostics-and-logs)
- [Roadmap / known limitations](#roadmap--known-limitations)
- [License](#license)

---

## Architecture

- **Daemon:** [`amneziawg-go`](https://github.com/amnezia-vpn/amneziawg-go) (a `wireguard-go` fork) — a userspace protocol implementation that creates a `tun` interface without any kernel involvement.
- **Configuration tool:** [`amneziawg-tools`](https://github.com/amnezia-vpn/amneziawg-tools) (`awg`) — talks to the daemon over a UAPI unix socket.
- **No kernel module is used.** Netgate does not publish the patched pfSense kernel sources for the 2.8.x branches publicly, which makes building an ABI-compatible kernel module (`if_awg.ko`) impossible without risking a kernel panic. A fully userspace architecture is the only viable approach for 2.8.1 at this time — this was a deliberate design decision, not an oversight.
- **Installation does not go through `pkg`/FreeBSD ports.** The plugin is installed by its own `install.sh` script, which copies files directly and registers the menu entry itself, bypassing the fragile `install_package_xml()` mechanism.
- **Tunnels are stored in a dedicated JSON file** (`/usr/local/etc/amnezia/amneziawg/tunnels.json`) rather than in pfSense's `config.xml`. Reason: a reproducible bug was found where `write_config()` on pfSense 2.8.1 fails to serialize deeply nested arbitrary-package data structures under any tested approach (see the comments in `awg.inc` for details). Simple entries (the menu item, the package record) are still written to `config.xml` — that works reliably.
- **Interface address/MTU/gateway are assigned exclusively through pfSense's own mechanism** (Interfaces → Assignments → Static IPv4), not directly by our code. After creating the `tun` device, the plugin calls pfSense's `interface_configure()`, which applies the already-saved configuration — a single source of truth, with no conflicts.

## Requirements

- pfSense 2.8.1-RELEASE (Community Edition).
- An external AmneziaWG server (address, port, keys, obfuscation parameters — typically provided by the server admin or exported from an Amnezia self-hosted panel).
- The `amneziawg-go` and `awg` binaries are **already included** in the repository (`bin/`) — built for FreeBSD 15.1/amd64. If your pfSense build is on a different architecture or a noticeably different kernel revision, build them yourself (see the repository issues or reach out through the feedback channel for build instructions) and replace the files under `bin/` before installing.

## Installation

```sh
pkg install git   # if not already installed
git clone https://github.com/qtronixx/pfSense-pkg-amneziawg-client.git
sh ./pfSense-pkg-amneziawg-client/install.sh install
```

After installation:
1. Verify the binaries are in place: `ls -la /usr/local/bin/amneziawg-go /usr/local/bin/awg`.
2. Open **VPN → AmneziaWG** in the pfSense web GUI — the menu entry and tunnel list page should appear.

## Update and removal

```sh
# Update (after git pull inside the cloned directory)
sh ./pfSense-pkg-amneziawg-client/install.sh update

# Full removal
sh ./pfSense-pkg-amneziawg-client/install.sh uninstall
```

Both flows stop the service on their own before replacing/removing the binaries — there's no need to kill the process manually.

## Setting up your first tunnel

1. **VPN → AmneziaWG → Add tunnel.**
2. Fill in the form manually, or use the **"Import from an existing .conf file"** block at the top of the form (see [below](#importing-an-existing-conf)).
3. Required fields:
   - **Private key** — paste your own, or click "Generate key pair".
   - **Obfuscation parameters** (`Jc/Jmin/Jmax/S1-S4/H1-H4/I1-I5`) — must **exactly** match the values on the server. `H1-H4` accept either a `min-max` range or a single number.
   - **Peers** — at least one peer with a required **Endpoint** (the external server's address and port). A peer without an Endpoint cannot be added through this form — that's how the client-only restriction of this package is technically enforced.
4. Save — the tunnel will appear in the list on the **Tunnels** page.
5. Click **"Apply changes"** — the `awgN` interface will be created and brought up.

## ⚠️ Mandatory pfSense GUI steps (traffic won't pass without them)

Creating and applying the tunnel in our plugin is **not enough** for actual traffic to pass — pfSense requires manual integration of the interface through its own built-in mechanisms. This isn't a bug, it's a mandatory requirement of pfSense's architecture for any VPN-type interface:

1. **Interfaces → Assignments** → add the newly appeared `awgN` to the list.
2. Open the created interface (e.g. `OPT1`) → give it a friendly name (e.g. `AWGCLIENT`).
3. **IPv4 Configuration Type: Static IPv4** → enter the address and mask provided by your server (e.g. `10.8.1.31/32`).
4. **IPv4 Upstream Gateway** → create a new gateway on that same address (the **"Add a new gateway"** button).
5. Enable interface → Save → Apply.
6. **⚠️ CRITICAL: Firewall → Rules → AWGCLIENT** → add an allow rule (`Allow all from any to any`, similar to the default LAN rule). **Without this rule, traffic through the tunnel will not pass at all**, even if the rest of your policy routing is configured correctly — pfSense blocks any traffic originating from a new interface until an explicit allow rule exists. This finding cost us several hours of debugging — don't skip this step.
7. For **full-tunnel** setups (`AllowedIPs = 0.0.0.0/0`), either make `AWGCLIENT` the default gateway, or set up a policy-routing rule on LAN (Destination → specific address/subnet → Gateway = the gateway you created).
8. **Firewall → NAT → Outbound** — in `automatic` mode, the NAT translation rule for `AWGCLIENT` will be created automatically once the interface has a real static address assigned (step 3).

### A note on MTU

By default, pfSense's `interface_configure()` sets the MTU to **1500** if the MTU field on the `AWGCLIENT` interface page is left blank. For WireGuard-like protocols, an MTU of **1420** is recommended (to account for encapsulation overhead) — if you run into fragmentation or large-packet-loss issues, enter `1420` explicitly in the **MTU** field on the **Interfaces → AWGCLIENT** page (not in our form — see Roadmap).

> 💡 **Important Note for Multiple AmneziaWG Connections**
> 
> If you are setting up connections to **multiple AmneziaWG servers simultaneously**, ensure that the VPN IP subnets on those servers do not overlap.
>
> **Why is this required?**  
> By default, AmneziaWG servers often assign the same client subnet (e.g., `10.8.1.0/24`). If two separate tunnels on pfSense receive IP addresses from the identical subnet, it will cause routing table conflicts, overlapping subnet routes, and broken gateway functionality.
>
> **How to change the subnet on your second server:**
> 1. Open the **AmneziaVPN** client application on your PC.
> 2. Go to **Settings** → **Servers** → Select your **2nd server**.
> 3. Click **Protocols** → **AmneziaWG** → **AmneziaWG Server Settings**.
> 4. In the **VPN IP Subnet** field, change the subnet to another unique private range (e.g., change `10.8.1.0/24` to `10.8.2.0/24` or `10.9.1.0/24`).
> 5. Save the configuration and generate a new config file for pfSense.


## Importing an existing `.conf`

On the **Add tunnel** page, there's an **"Import from an existing .conf file"** block:
- **Paste** the contents of your config (the native AmneziaWG format with `[Interface]`/`[Peer]` sections) into the text box, or
- **Upload a file** using the file picker — its contents will be pasted into the text box automatically.

Click **"Parse and fill the form"** — all fields will be populated automatically. Parsing **does not save** the tunnel — review/adjust the values and then click the regular **"Save"** button.

## Diagnostics and logs

```sh
# Service status
service awg status

# Plugin's PHP debug logs (currently always on, see Roadmap)
grep "AmneziaWG DEBUG" /var/log/system.log | tail -50

# Real tunnel status at the protocol level
awg show all

# Tunnel storage (JSON, not config.xml)
cat /usr/local/etc/amnezia/amneziawg/tunnels.json

# A specific daemon's log
cat /var/run/amneziawg/awgN.log
```

## Roadmap / known limitations

Open tasks currently being worked on:

- [x] ~~**A checkbox to toggle debug logging** on the **VPN → AmneziaWG → Tunnels** page — currently `AWG_DEBUG = true` is hardcoded in `awg.inc`, meaning debug messages are always written to the system log.~~ *(Implemented in v1.0.1)*
- [ ] **Programmatic MTU application** — MTU currently has to be set manually on the pfSense interface page (see above); the plan is to automatically apply the recommended `1420` value after `interface_configure()`.
- [ ] **The "Tunnel address" field in the edit form** — currently has no effect on the actual configuration (the address is assigned via the pfSense Interfaces GUI) and needs a decision: either remove the field or clearly mark it as informational/legacy.
- [ ] Automatic integration with the pfSense DNS Resolver — DNS servers from an imported config are currently only logged; applying them is a manual step via **System → General Setup**.
- [ ] Testing on pfSense architectures/builds other than the one used during development (amd64, FreeBSD 15.1-based build).

If you run into an issue not covered here, please open an [Issue](../../issues) with a description, your pfSense version, and logs from the "Diagnostics" section above.

## License

MIT — see [LICENSE](LICENSE).

## 🤝 Contributors & Acknowledgments

* **[Dmitry](https://github.com/qtronixx)** — Lead Developer & Project Maintainer
* ![Claude](https://img.shields.io/badge/Anthropic%20Claude-D97757?style=flat-square&logo=claude&logoColor=white) — Core code development & implementation
* ![DeepSeek](https://img.shields.io/badge/DeepSeek-4D6BFE?style=flat-square&logo=deepseek&logoColor=white) — Code review & deep analysis
* ![Gemini](https://img.shields.io/badge/Google%20Gemini-8E75B2?style=flat-square&logo=googlegemini&logoColor=white) — Architecture, workflow logic & debugging
