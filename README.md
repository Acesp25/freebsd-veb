# FreeBSD Virtual Ethernet Bridge (veb)
`veb` is a virtual Ethernet bridge pseudo driver for FreeBSD, inspired by OpenBSD's `veb(4)`. It is a learning L2 forwarding plane built for the topology bhyve guests and vnet jails produce: virtual interfaces on a single host, no physical uplink, no untrusted cabling. The host cannot communicate with a veb segment unless a `vport` interface is attached.

`vport` is the companion driver providing explicit host presence. Both network interface drivers live in `if_veb.ko`.

Start with [overview.md](docs/overview.md) for why this exists and how it differs from `if_bridge(4)`.

This project is currently experimental and out of tree. Development and testing is being done on CURRENT.

[![Build Status](https://github.com/Acesp25/freebsd-veb/actions/workflows/vmactions.yml/badge.svg)](https://github.com/Acesp25/freebsd-veb/actions/workflows/vmactions.yml)

## Requirements
- Root for everything below: loading the module, creating interfaces, and running the test suite.

## Known limitation: coexistence with `if_bridge(4)`

> **If `if_bridge.ko` is loaded before `if_veb.ko`, destroying an interface that is still a veb member panics the kernel.**
>
> Both drivers store a member backpointer in `ifp->if_bridge`, and both register a handler on `ifnet_departure_event` at `EVENTHANDLER_PRI_ANY`, so the handlers run in module load order. `veb_delete_member()` clears `ifp->if_bridge`, so when `if_veb` is loaded first its handler runs first and `bridge_ifdetach()` sees NULL. In the reverse order `bridge_ifdetach()` reads a live `struct veb_port *` as a `struct bridge_iflist *` and panics in `_sx_xlock()`.
>
> `veb_port_of()` checks ownership before trusting the field, so `veb` never misreads a bridge member. `bridge_ifdetach()` has no equivalent check, which is why only one direction fails.
>
> Until an ownership gate is applied to `bridge_ifdetach()`, load `if_veb.ko` before `if_bridge.ko`, or remove members from their veb before destroying them.

## Building
From the repository root:
```sh
make            # build the driver
make vebctl     # build the vebctl control utility
```
Build products (`if_veb.ko`, `vebctl`) land in `obj/`.

## Loading
```sh
kldload obj/if_veb.ko
kldunload if_veb
```

## Usage
Create the interfaces:
```sh
ifconfig veb create
ifconfig vport create
```

`vebctl` drives the control plane. Run it with no arguments for the full list of subcommands.

```sh
./obj/vebctl veb0 add vport0                         # add vport0 to veb0
./obj/vebctl veb0 del vport0                         # remove vport0 from veb0
./obj/vebctl veb0 show                               # member list (VEBGIFS)
./obj/vebctl veb0 rts                                # forwarding table (VEBGRTS)
./obj/vebctl veb0 flags vport0                       # member flags (VEBGIFFLGS)
./obj/vebctl veb0 setflags vport0 +sticky -discover  # adjust member flags (VEBSIFFLGS)
./obj/vebctl veb0 timeout                            # cache timeout (VEBGTO)
./obj/vebctl veb0 timeout 240                        # set cache timeout (VEBSTO)
```

A member may not carry an IP address. Add the `vport` to the veb first, then address it:

```sh
ifconfig veb0 create
ifconfig vport0 create
./obj/vebctl veb0 add vport0
ifconfig vport0 192.0.2.1/24 up
```

## Tunables

| sysctl                             |Default|Effect|
| ---------------------------------- | ------- | --------------------------------------------- |
| `net.link.veb.inherit_mac`         |0|Inherit the MAC address of the first member|
| `net.link.veb.log_mac_flap`        |1|Log MAC addresses moving between ports|
| `net.link.veb.rtable_prune_period` |300|Seconds between walks of the forwarding table|

`rtable_prune_period` currently has no lower bound; setting it to zero leaves the callout rescheduling continuously.

## Testing
The suite uses ATF and Kyua, and also needs root.

> **Load `if_veb.ko` before running the suite.** `bridge_mutual_exclusion` loads `if_bridge.ko` and leaves it loaded, which is harmless as long as `if_veb.ko` was loaded first. If `if_bridge.ko` is already loaded before loading `if_veb.ko`, `member_departure` will panic the kernel.

```sh
kldload obj/if_veb.ko
cd tests/
kyua test
```

## Documentation
Every document here lives in the docs/ directory.
- [overview.md](docs/overview.md) - what this is and why it is not `if_bridge` with removed features
- [design.md](docs/design.md) - design decisions, with stable IDs for reference
- [arch.md](docs/arch.md) - the datapath, step by step, for both veb and vport
- [roadmap.md](docs/roadmap.md) - what is planned, and what is not
- [contributing.md](docs/contributing.md) - how to contribute

## License
BSD-3-Clause. Copyright 2026 Aaron Espinoza [acesp25@FreeBSD.org](mailto:acesp25@FreeBSD.org).
