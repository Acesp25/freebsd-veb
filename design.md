# Design decisions
This document covers where `veb(4)` deliberately diverges from `if_bridge(4)`, and why. 

Decisions carry stable IDs (`D1`, `D2`, ...) that can easily be referenced in discussions and threads.

Each entry gives the decision, why, what it costs, and how it is verified.

## Index
**Host presence**
- [D1](#d1-no-implicit-host-presence) — No implicit host presence
- [D2](#d2-explicit-l2-host-presence-via-vport-a-first-class-cloner) — Explicit L2 host presence via `vport`, a first-class cloner
- [D3](#d3-if_bridge-reused-as-the-member-backpointer-vport-excluded) — `if_bridge` reused as the member backpointer; vport excluded

**Membership**
- [D4](#d4-members-may-not-carry-ip-addresses) — Members may not carry IP addresses
- [D5](#d5-one-vport-per-veb-ifveb_hasvport) — One vport per veb (`IFVEB_HASVPORT`)

**Control plane and ABI**
- [D6](#d6-direction-based-default-deny-on-member-ioctls) — Direction-based default-deny on member ioctls
- [D7](#d7-private-minimal-abi) — Private minimal ABI

**Non-goals**
- [D8](#d8-no-stp) — No STP
- [D9](#d9-no-pfil) — No pfil
- [D10](#d10-no-span-ports) — No span ports
---

## Host presence
### D1: No implicit host presence
- `veb_transmit()` returns `ENETDOWN`. No `GRAB_OUR_PACKETS`, no reinjection into `ether_input()`, and `veb_input()` returns NULL once `veb_forward()` has taken the frame, so there is no local delivery on the receiving member. The host reaches a segment only through a vport (D2).
- **Why:** the host stack has no business on a segment it only switches. It also drops the two per-broadcast-frame copies and the `m_copyup()` that `if_bridge` pays; see `overview.md`.
- Because nothing reinjects, `veb_forward()` taps BPF unconditionally: no other layer would ever see the frame, and there is no double tap to avoid. Adding reinjection later would break that.
- **Cost:** no host access until a vport exists, which differs from `if_bridge` behaviour.
- **Tested:** `no_host_presence`, with `transmit_ipv4_unicast` as the positive control.

### D2: Explicit L2 host presence via `vport`, a first-class cloner
- Host presence is its own cloned interface rather than a property of the veb ifnet.
- **Why:** a vport has its own flags, counters, MTU and addresses, can move into a vnet jail, and can be absent entirely. None of that is available if the host is on the segment by default.
- `vport_transmit()` routes through `veb_forward()` rather than shortcutting to the members, so the host MAC is learned like any other source and the learning table needs no special case.
- **Cost:** one more interface to configure. `vport_ioctl()` rejects most configuration while `sc_vp == NULL`, so a vport is not useful standalone.
- **Tested:** `vport_host_path`

### D3: `if_bridge` reused as the member backpointer; vport excluded
- Members keep their `struct veb_port` backpointer in `ifp->if_bridge`. The vport does not; its association lives in `vport_softc->sc_vp`, and `veb_port_of()` resolves a vport by comparing its ioctl function pointer.
- **Why:** `ether_output()` and `ether_input()` dispatch on `if_bridge` first (the `BRIDGE_INPUT()` site in `if_ethersubr.c`). Setting it on a vport would either bypass `if_transmit()` or loop frames back into forwarding.
- **Cost 1:** the field has no owner tag. `bridge_ifdetach()` and `veb_ifdetach()` both run on the global `ifnet_departure_event` and both cast `ifp->if_bridge` to their own type; `struct bridge_iflist` and `struct veb_port` share their first three fields, so the softc pointer reads back as valid and gets locked. Destroying an interface that is still a veb member, with `if_bridge.ko` loaded, panics in `_sx_xlock()` from `bridge_ifdetach()`. Both handlers must gate on `if_bridge_input` before trusting the field; the `if_bridge` side is a separate commit in the series.
- **Cost 2:** `ifhwioctl()`'s "no MTU changes on bridge members" guard keys off `if_bridge`, so it misses the vport and `vport_ioctl()` enforces that itself.
- **Tested:** `bridge_mutual_exclusion` (join-side exclusion), `delete_with_members` (field cleared on teardown), `member_departure` (regression for the panic, but only with `if_bridge.ko` loaded).

## Membership
### D4: Members may not carry IP addresses
- `veb_ioctl_add()` walks `if_addrhead` and rejects any `AF_INET` or `AF_INET6` address; `veb_p_ioctl()` maintains it afterwards.
- **Why:** a member is a switch port, not a host interface. `if_bridge` handles the same problem at runtime with `bridge_member_ifaddrs_p` and a sysctl; rejecting at add time leaves no half-configured state.
- IPv6 is rejected on the same footing as IPv4, under `ipv6_activate_all_interfaces`, where an interface picks up a link-local address with no operator action and then fails to enroll.
- Addresses belong on a vport (D2), assigned after it joins the veb. `vport_ioctl()` refuses address configuration while `sc_vp` is NULL, so the ordering is enforced from both sides.
- **Cost:** configurations that address a bridge member do not port over unchanged.
- **Tested:** `add_errors`, `vport_host_path` (ordering). IPv6 not covered; `learning_expire` has the `inet6 ifdisabled` idiom to borrow.

### D5: One vport per veb (`IFVEB_HASVPORT`)
- **Why:** a consequence of not yet having VLAN filtering, not a principled limit. With one untagged segment per veb there is nothing for a second vport to attach to.
- **Cost:** a host wanting presence on several segments needs several vebs. Expected to change once VLAN filtering lands; see [roadmap.md](roadmap.md).
- **Tested:** `vport_constraints`

## Control plane and ABI
### D6: Direction-based default-deny on member ioctls
- Members get their `if_ioctl` replaced with `veb_p_ioctl()`, which denies any command with `IOC_IN` set unless allowlisted. `SIOCSIFFLAGS`, `SIOCSIFMTU`, `SIOCADDMULTI` and `SIOCDELMULTI` are allowed; `SIOCSIFADDR`, `SIOCAIFADDR` and `SIOCSIFCAP` are denied by name. `if_bridge` has no member ioctl shim at all.
- **Why:** it fails closed as the kernel gains new `SIOC*` commands. A denylist would silently permit anything added later. This picked up `SIOCAIFADDR_IN6` for free.
- **Cost:** `veb_set_ifcap()` bypasses the shim, commented at the call site. It is the only bypass.
- **Tested:** not covered. `ioctl_validation` covers the `SIOCSDRVSPEC` dispatch table (D7), a different mechanism. `ifconfig <member> mtu 1400` or an address add against a member would cover this.

### D7: Private minimal ABI
- `VEBADD` / `VEBDEL` / `VEBGIFS` and `struct ifvreq`, not the `BRDG*` surface. Eight subcommands against bridge's thirty-nine, dispatched through `veb_control_table[]` with per-entry argument size, direction and privilege flags.
- **Why:** an ABI is a promise. Promising `BRDGSPRI` in a driver with no STP is worse than not offering it.
- Port flags are set and cleared by mask, `vp->vp_flags = (vp->vp_flags & ~clrmask) | setmask`, applied to the port's flags rather than the userland-supplied value, with both masks validated against `IFVPUMASK` and a flag in both rejected. `if_bridge`'s read-modify-write on the supplied value races two concurrent flag changes against each other; deltas mean two callers changing different flags cannot clobber one another.
- `VEBGIFS` keeps `BRDGGIFS`'s two-pass copyout protocol; deviating would buy nothing.
- **Cost:** `ifconfig` needs its own `ifveb.c`, with distinct subcommand names to avoid the flat global `cmd_register` collision.
- **Tested:** `ioctl_validation`, `unprivileged`, `flags_roundtrip`. `vport_flag_rejected` skips without a `rawflags` verb in `vebctl`; do not count it until it runs.

## Non-goals
Decisions, not unfinished work. Merely unwritten features live in [roadmap.md](roadmap.md).
### D8: No STP
- **Why:** STP exists because someone can plug a cable into the wrong port. There are no cables here, and the forwarding delay is a real cost on guest and jail startup.
- **Cost:** because `veb` accepts `IFT_ETHER` members, an operator has the ability to create a loop. This is not something we prevent; we note that it is possible.
- **Tested:** n/a

### D9: No pfil
- **Why:** filtering belongs at the tap or inside the guest, where the policy has context. A filter hook inside an L2 forwarding plane sees frames the hypervisor operator cannot reason about.
- **Cost:** configurations that filter at the bridge do not port over.
- **Tested:** n/a

### D10: No span ports
- **Why:** `veb_forward()` taps unconditionally (D1), so `tcpdump(1)` on the veb sees every forwarded frame without a dedicated span port.
- **Cost:** no way to mirror a segment onto a separate interface for an external collector.
- **Tested:** n/a
