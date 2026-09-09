# Roadmap
Features that are deliberately absent are not here, they might be in [design.md](design.md) under non-goals.

## Planned
VLAN filtering is the next substantial piece of work. Multiple vports per veb (D5) follows directly from it: the one-vport limit exists because there is no way to separate segments within a veb, not because one vport is the right number.

The rest are smaller gaps, order is not a guarantee for what is getting implemented next.

- [ ] A control path for `vp_addrmax`. `veb_rtupdate()` and `veb_forward()` both enforce it and nothing sets it, so it is permanently zero.
- [ ] Implement user-facing flush and static-address subcommands: `IFVF_FLUSHDYN`, `IFVF_FLUSHALL` and `IFVAF_STATIC`. The flags exist internally; nothing in the control plane reaches them.
- [ ] An `iflladdr` handler, so rtnodes do not go stale when a member's link-layer address changes underneath them.
- [ ] `IFVEB_HASVPORT` exposed to userland.
- [ ] Per-port, per-CPU counters, similar to how OpenBSD does it.
- [ ] A lower clamp on the prune period sysctl (D12). It is currently settable to values that make no sense.
- [ ] Test coverage for the member ioctl shim (D6). The default-deny direction check is the kind of thing that fails silently when the kernel gains a new `SIOC*` command.
- [ ] A `rawflags` verb in the control harness, so the `vport_flag_rejected` path is actually exercised.
- [ ] The IPv6 case in D4. Members are rejected for carrying addresses.

## In-tree integration
These are important if/when we submit this driver to be implemented into the src. They do not deal with the driver's code directly but are cases where we should modify existing code to allow veb to comfortably exist in the src.

- [ ] `sbin/ifconfig/ifveb.c`, replacing the standalone control harness as the user interface.
- [ ] The `bridge_ifdetach()` ownership gate.
- [ ] `EXTERR_CAT_VEB` in `sys/sys/exterrvar.h`.
- [ ] `veb.4` finished.

---

If something seems to be missing or if there are any suggestions, feel free to create an issue.
