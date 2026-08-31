#!/usr/libexec/atf-sh
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright 2026 Aaron Espinoza <acesp25@FreeBSD.org>
#
# ATF/Kyua tests for if_veb(4).
#
# Two layers of coverage:
#   - control plane: SIOCSDRVSPEC/SIOCGDRVSPEC subcommands driven through
#     vebctl, including the dispatch-table validation (bounds, direction,
#     argument size, privilege).
#   - data plane: forwarding behavior through epair members owned by vnet
#     jails, including the deliberate no-host-presence semantics and the
#     vport host path.
#
# Structure modeled on src's tests/sys/net/if_bridge_test.sh, but self
# contained: helpers live in veb.subr next to this file.

. $(atf_get_srcdir)/veb.subr

# ---------------------------------------------------------------------
# Control plane
# ---------------------------------------------------------------------

atf_test_case "create_destroy" "cleanup"
create_destroy_head()
{
	atf_set descr 'veb interfaces can be created and destroyed'
	atf_set require.user root
}
create_destroy_body()
{
	veb_init

	veb=$(veb_mkveb)
	atf_check -s exit:0 -o ignore ifconfig ${veb}

	# A fresh veb has no members and the default cache timeout.
	atf_check -s exit:0 -o match:"no members" ${VEBCTL} ${veb} show
	atf_check -s exit:0 -o match:"cache timeout 1200 seconds" \
	    ${VEBCTL} ${veb} timeout
}
create_destroy_cleanup()
{
	veb_cleanup
}

atf_test_case "member_add_del" "cleanup"
member_add_del_head()
{
	atf_set descr 'members can be added and removed; new members get' \
	    ' LEARNING|DISCOVER'
	atf_set require.user root
}
member_add_del_body()
{
	veb_init

	veb=$(veb_mkveb)
	ep=$(veb_mkepair)

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep}a
	atf_check -s exit:0 -o match:"${ep}a" ${VEBCTL} ${veb} show

	# Default port flags: IFVP_LEARNING|IFVP_DISCOVER == 0x3.
	atf_check -s exit:0 -o match:"flags=0x3<learning,discover>" \
	    ${VEBCTL} ${veb} flags ${ep}a

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} del ${ep}a
	atf_check -s exit:0 -o match:"no members" ${VEBCTL} ${veb} show
}
member_add_del_cleanup()
{
	veb_cleanup
}

atf_test_case "add_errors" "cleanup"
add_errors_head()
{
	atf_set descr 'VEBADD/VEBDEL error paths: ENOENT, EEXIST, EBUSY,' \
	    ' EINVAL for addressed members'
	atf_set require.user root
}
add_errors_body()
{
	veb_init

	veb=$(veb_mkveb)
	veb2=$(veb_mkveb)
	ep=$(veb_mkepair)

	# Nonexistent interface -> ENOENT.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} add nonexistent0

	# Deleting a non-member -> ENOENT.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} del ${ep}a

	# A member carrying an IP address is rejected...
	ifconfig ${ep}a 192.0.2.5/24
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} add ${ep}a

	# ... and accepted once the address is gone.
	ifconfig ${ep}a 192.0.2.5 -alias
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep}a

	# Adding it twice -> EEXIST.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} add ${ep}a

	# Membership is exclusive across vebs -> EBUSY.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb2} add ${ep}a
}
add_errors_cleanup()
{
	veb_cleanup
}

atf_test_case "bridge_mutual_exclusion" "cleanup"
bridge_mutual_exclusion_head()
{
	atf_set descr 'an if_bridge member cannot join a veb and vice versa'
	atf_set require.user root
}
bridge_mutual_exclusion_body()
{
	veb_init
	veb_bridge_init

	veb=$(veb_mkveb)
	bridge=$(veb_mkbridge)
	ep=$(veb_mkepair)
	ep2=$(veb_mkepair)

	# bridge member -> veb add must fail (if_bridge != NULL).
	atf_check -s exit:0 -o ignore ifconfig ${bridge} addm ${ep}a
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} add ${ep}a

	# veb member -> bridge addm must fail for the same reason.
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep2}a
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ifconfig ${bridge} addm ${ep2}a
}
bridge_mutual_exclusion_cleanup()
{
	veb_cleanup
}

atf_test_case "ioctl_validation" "cleanup"
ioctl_validation_head()
{
	atf_set descr 'dispatch table validation: subcommand bounds,' \
	    ' transfer direction, argument size'
	atf_set require.user root
}
ioctl_validation_body()
{
	veb_init

	veb=$(veb_mkveb)
	ep=$(veb_mkepair)

	# The control table has 8 entries; 8 is the first out-of-bounds
	# subcommand, and a large value must fail identically.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} -c 8 ${veb} add ${ep}a
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} -c 99 ${veb} add ${ep}a

	# Direction enforcement: a set-only subcommand sent as
	# SIOCGDRVSPEC, and a copyout subcommand sent as SIOCSDRVSPEC.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} -g ${veb} add ${ep}a
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} -s ${veb} show

	# Argument size enforcement.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} -l 4 ${veb} add ${ep}a

	# None of the rejected requests may have had side effects.
	atf_check -s exit:0 -o match:"no members" ${VEBCTL} ${veb} show
}
ioctl_validation_cleanup()
{
	veb_cleanup
}

atf_test_case "unprivileged" "cleanup"
unprivileged_head()
{
	atf_set descr 'VC_F_SUSER subcommands reject unprivileged users;' \
	    ' read-only subcommands do not'
	atf_set require.user root
}
unprivileged_body()
{
	veb_init

	veb=$(veb_mkveb)
	ep=$(veb_mkepair)

	# Read path carries no VC_F_SUSER: allowed for nobody.
	atf_check -s exit:0 -o ignore \
	    env SHELL=/bin/sh su -m nobody -c "${VEBCTL} ${veb} show"

	# Write path must be denied.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    env SHELL=/bin/sh su -m nobody -c "${VEBCTL} ${veb} add ${ep}a"
	atf_check -s exit:0 -o match:"no members" ${VEBCTL} ${veb} show
}
unprivileged_cleanup()
{
	veb_cleanup
}

atf_test_case "flags_roundtrip" "cleanup"
flags_roundtrip_head()
{
	atf_set descr 'VEBSIFFLGS set/clear masks apply correctly and' \
	    ' overlapping masks are rejected'
	atf_set require.user root
}
flags_roundtrip_body()
{
	veb_init

	veb=$(veb_mkveb)
	ep=$(veb_mkepair)

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep}a

	# 0x3<learning,discover> -> +sticky -discover ->
	# 0x5<learning,sticky>.  This is the regression test for the
	# "&= vs |=" and "modified req instead of vp_flags" bugs.
	atf_check -s exit:0 -o match:"flags=0x5<learning,sticky>" \
	    ${VEBCTL} ${veb} setflags ${ep}a +sticky -discover

	# The change must persist across an unrelated get.
	atf_check -s exit:0 -o match:"flags=0x5<learning,sticky>" \
	    ${VEBCTL} ${veb} flags ${ep}a

	# A flag in both masks -> EINVAL, and flags are untouched.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} setflags ${ep}a +private -private
	atf_check -s exit:0 -o match:"flags=0x5<learning,sticky>" \
	    ${VEBCTL} ${veb} flags ${ep}a

	# Flags on an unknown member -> ENOENT.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} flags ${ep}b
}
flags_roundtrip_cleanup()
{
	veb_cleanup
}

atf_test_case "vport_flag_rejected" "cleanup"
vport_flag_rejected_head()
{
	atf_set descr 'IFVP_VPORT is not user-settable on an ordinary' \
	    ' member (requires IFVPUMASK enforcement in the driver and' \
	    ' a rawflags verb in vebctl)'
	atf_set require.user root
}
vport_flag_rejected_body()
{
	veb_init

	if ! ${VEBCTL} 2>&1 | grep -q rawflags; then
		atf_skip "vebctl lacks a rawflags verb"
	fi

	veb=$(veb_mkveb)
	ep=$(veb_mkepair)

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep}a

	# IFVP_VPORT == 0x10.  The kernel must reject this: a member
	# carrying the bit takes the host-delivery branch in
	# veb_enqueue() and gets its softc miscast in
	# veb_delete_member().  Fails until veb_ioctl_sifflags()
	# validates against IFVPUMASK instead of IFVPMASK.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} rawflags ${ep}a 0x10 0x0

	# A truly unknown bit outside IFVPMASK must also be rejected.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} rawflags ${ep}a 0x100 0x0

	atf_check -s exit:0 -o match:"flags=0x3<learning,discover>" \
	    ${VEBCTL} ${veb} flags ${ep}a
}
vport_flag_rejected_cleanup()
{
	veb_cleanup
}

atf_test_case "timeout_roundtrip" "cleanup"
timeout_roundtrip_head()
{
	atf_set descr 'VEBGTO/VEBSTO cache timeout get/set'
	atf_set require.user root
}
timeout_roundtrip_body()
{
	veb_init

	veb=$(veb_mkveb)

	atf_check -s exit:0 -o match:"cache timeout 1200 seconds" \
	    ${VEBCTL} ${veb} timeout
	atf_check -s exit:0 -o match:"cache timeout 240 seconds" \
	    ${VEBCTL} ${veb} timeout 240

	# Per-instance state: a second veb keeps its own default.
	veb2=$(veb_mkveb)
	atf_check -s exit:0 -o match:"cache timeout 1200 seconds" \
	    ${VEBCTL} ${veb2} timeout
}
timeout_roundtrip_cleanup()
{
	veb_cleanup
}

atf_test_case "mtu" "cleanup"
mtu_head()
{
	atf_set descr 'first member defines the veb MTU; later members' \
	    ' are coerced to it'
	atf_set require.user root
}
mtu_body()
{
	veb_init

	veb=$(veb_mkveb)
	ep1=$(veb_mkepair)
	ep2=$(veb_mkepair)

	ifconfig ${ep1}a mtu 9000

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep1}a
	atf_check -s exit:0 -o match:"mtu 9000" ifconfig ${veb}

	# ep2 comes in at 1500; the driver issues SIOCSIFMTU to align it.
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep2}a
	atf_check -s exit:0 -o match:"mtu 9000" ifconfig ${ep2}a
}
mtu_cleanup()
{
	veb_cleanup
}

atf_test_case "delete_with_members" "cleanup"
delete_with_members_head()
{
	atf_set descr 'destroying a veb with members releases them cleanly'
	atf_set require.user root
}
delete_with_members_body()
{
	veb_init

	veb=$(ifconfig veb create)
	ep=$(veb_mkepair)
	ep2=$(veb_mkepair)

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep}a
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep2}a

	# Deliberately not tracked by veb_mkveb: destroy it by hand while
	# it still holds members.
	atf_check -s exit:0 ifconfig ${veb} destroy

	# The members must be fully released (if_bridge cleared, ioctl
	# vector restored): re-adding one to a fresh veb has to work.
	veb2=$(veb_mkveb)
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb2} add ${ep}a
	atf_check -s exit:0 -o match:"${ep}a" ${VEBCTL} ${veb2} show
}
delete_with_members_cleanup()
{
	veb_cleanup
}

atf_test_case "member_departure" "cleanup"
member_departure_head()
{
	atf_set descr 'destroying a member interface removes it from the' \
	    ' veb via the ifnet departure event'
	atf_set require.user root
}
member_departure_body()
{
	veb_init

	veb=$(veb_mkveb)
	ep=$(ifconfig epair create)
	ep=${ep%a}

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep}a
	atf_check -s exit:0 ifconfig ${ep}a destroy

	atf_check -s exit:0 -o match:"no members" ${VEBCTL} ${veb} show

	# The veb itself must remain fully functional afterwards.
	ep2=$(veb_mkepair)
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep2}a
}
member_departure_cleanup()
{
	veb_cleanup
}

# ---------------------------------------------------------------------
# Data plane
# ---------------------------------------------------------------------

atf_test_case "transmit_ipv4_unicast" "cleanup"
transmit_ipv4_unicast_head()
{
	atf_set descr 'IPv4 forwarding between two jailed epair members'
	atf_set require.user root
}
transmit_ipv4_unicast_body()
{
	veb_init
	vnet_init

	ep_one=$(veb_mkepair)
	ep_two=$(veb_mkepair)

	veb_mkjail one ${ep_one}b
	veb_mkjail two ${ep_two}b

	jexec one ifconfig ${ep_one}b 192.0.2.1/24 up
	jexec two ifconfig ${ep_two}b 192.0.2.2/24 up

	veb=$(veb_mkveb)
	ifconfig ${veb} up
	ifconfig ${ep_one}a up
	ifconfig ${ep_two}a up
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep_one}a
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep_two}a

	# ARP resolution exercises broadcast flooding; the echo exchange
	# exercises learned unicast forwarding.
	atf_check -s exit:0 -o ignore jexec one ping -c 3 -t 1 192.0.2.2
	atf_check -s exit:0 -o ignore jexec two ping -c 3 -t 1 192.0.2.1
}
transmit_ipv4_unicast_cleanup()
{
	veb_cleanup
}

atf_test_case "no_host_presence" "cleanup"
no_host_presence_head()
{
	atf_set descr 'the veb interface itself neither sources nor sinks' \
	    ' traffic: no implicit host presence by design'
	atf_set require.user root
}
no_host_presence_body()
{
	veb_init
	vnet_init

	ep_one=$(veb_mkepair)
	ep_two=$(veb_mkepair)

	veb_mkjail one ${ep_one}b
	veb_mkjail two ${ep_two}b

	jexec one ifconfig ${ep_one}b 192.0.2.2/24 up
	jexec two ifconfig ${ep_two}b 192.0.2.3/24 up

	veb=$(veb_mkveb)
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep_one}a
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep_two}a
	ifconfig ${ep_one}a up
	ifconfig ${ep_two}a up

	# Address the veb itself, as one would an if_bridge SVI.
	ifconfig ${veb} 192.0.2.1/24 up

	# Positive control: forwarding between the jails works.
	atf_check -s exit:0 -o ignore jexec one ping -c 3 -t 1 192.0.2.3

	# veb_transmit() returns ENETDOWN: the host cannot reach the
	# segment through the veb address...
	atf_check -s not-exit:0 -o ignore -e ignore ping -c 1 -t 2 192.0.2.2

	# ... and with no GRAB_OUR_PACKETS equivalent, members cannot
	# reach the host either.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    jexec one ping -c 1 -t 2 192.0.2.1
}
no_host_presence_cleanup()
{
	veb_cleanup
}

atf_test_case "vport_host_path" "cleanup"
vport_host_path_head()
{
	atf_set descr 'a vport member provides the explicit host data path'
	atf_set require.user root
}
vport_host_path_body()
{
	veb_init
	vnet_init

	ep=$(veb_mkepair)
	veb_mkjail one ${ep}b
	jexec one ifconfig ${ep}b 192.0.2.2/24 up

	veb=$(veb_mkveb)
	vport=$(veb_mkvport)

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep}a
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${vport}

	# Address the vport only after it joins: VEBADD rejects members
	# that already carry an address, vport included.
	ifconfig ${vport} 192.0.2.1/24 up
	ifconfig ${ep}a up
	ifconfig ${veb} up

	atf_check -s exit:0 -o match:"vport" ${VEBCTL} ${veb} flags ${vport}

	atf_check -s exit:0 -o ignore ping -c 3 -t 1 192.0.2.2
	atf_check -s exit:0 -o ignore jexec one ping -c 3 -t 1 192.0.2.1
}
vport_host_path_cleanup()
{
	veb_cleanup
}

atf_test_case "vport_constraints" "cleanup"
vport_constraints_head()
{
	atf_set descr 'one vport per veb; a vport belongs to at most one veb'
	atf_set require.user root
}
vport_constraints_body()
{
	veb_init

	veb=$(veb_mkveb)
	veb2=$(veb_mkveb)
	vport=$(veb_mkvport)
	vport2=$(veb_mkvport)

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${vport}

	# Second vport on the same veb -> EINVAL (IFVEB_HASVPORT).
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb} add ${vport2}

	# Same vport on another veb -> EBUSY (sc_vp already set).
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb2} add ${vport}

	# After removal both constraints release.
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} del ${vport}
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb2} add ${vport}
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${vport2}

	# Destroying a vport that is still a member must detach it via
	# the ifnet departure event: vport_clone_destroy() asserts
	# sc_vp == NULL only after ether_ifdetach() has run it.
	atf_check -s exit:0 ifconfig ${vport} destroy
	atf_check -s not-exit:0 -o ignore -e ignore \
	    ${VEBCTL} ${veb2} flags ${vport}
	atf_check -s exit:0 -o match:"no members" ${VEBCTL} ${veb2} show

	# The departure path must also have cleared IFVEB_HASVPORT, so
	# veb2 accepts a replacement vport.
	vport3=$(veb_mkvport)
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb2} add ${vport3}
}
vport_constraints_cleanup()
{
	veb_cleanup
}

atf_test_case "learning_expire" "cleanup"
learning_expire_head()
{
	atf_set descr 'dynamic entries are learned on the correct member' \
	    ' and honor the cache timeout'
	atf_set require.user root
}
learning_expire_body()
{
	veb_init
	vnet_init

	# Shorten the prune walk so real entry removal is observable.
	# Must happen before the veb is brought up: veb_init() arms the
	# callout with the value current at that moment.  Saved to a file
	# so the cleanup routine can restore it.
	sysctl -n net.link.veb.veb_rtable_prune_period > prune_period.old
	sysctl net.link.veb.veb_rtable_prune_period=2

	ep_one=$(veb_mkepair)
	ep_two=$(veb_mkepair)

	veb_mkjail one ${ep_one}b
	veb_mkjail two ${ep_two}b

	# Keep IPv6 ND from refreshing entries mid-test.
	jexec one ifconfig ${ep_one}b inet6 ifdisabled
	jexec two ifconfig ${ep_two}b inet6 ifdisabled
	jexec one ifconfig ${ep_one}b 192.0.2.1/24 up
	jexec two ifconfig ${ep_two}b 192.0.2.2/24 up

	veb=$(veb_mkveb)
	ifconfig ${veb} up
	ifconfig ${ep_one}a up
	ifconfig ${ep_two}a up
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep_one}a
	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${ep_two}a

	atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} timeout 3

	atf_check -s exit:0 -o ignore jexec one ping -c 2 -t 1 192.0.2.2

	# Both source MACs must have been learned, dynamically, on the
	# member they arrived on.
	mac_one=$(jexec one ifconfig ${ep_one}b | awk '/ether/ {print $2}')
	mac_two=$(jexec two ifconfig ${ep_two}b | awk '/ether/ {print $2}')
	atf_check -s exit:0 -o match:"${mac_one}.*${ep_one}a.*dynamic" \
	    ${VEBCTL} ${veb} rts
	atf_check -s exit:0 -o match:"${mac_two}.*${ep_two}a.*dynamic" \
	    ${VEBCTL} ${veb} rts

	# With timeout 3 and a 2-second prune walk, both entries must be
	# aged out and actually removed well within 8 seconds.
	sleep 8
	atf_check -s exit:0 -o match:"forwarding table is empty" \
	    ${VEBCTL} ${veb} rts
}
learning_expire_cleanup()
{
	if [ -f prune_period.old ]; then
		sysctl \
		    net.link.veb.veb_rtable_prune_period=$(cat prune_period.old)
		rm prune_period.old
	fi
	veb_cleanup
}

atf_test_case "private_ports" "cleanup"
private_ports_head()
{
	atf_set descr 'IFVP_PRIVATE members do not exchange traffic with' \
	    ' each other'
	atf_set require.user root
}
private_ports_body()
{
	veb_init
	vnet_init

	ep_one=$(veb_mkepair)
	ep_two=$(veb_mkepair)
	ep_three=$(veb_mkepair)

	veb_mkjail one ${ep_one}b
	veb_mkjail two ${ep_two}b
	veb_mkjail three ${ep_three}b

	jexec one ifconfig ${ep_one}b 192.0.2.1/24 up
	jexec two ifconfig ${ep_two}b 192.0.2.2/24 up
	jexec three ifconfig ${ep_three}b 192.0.2.3/24 up

	veb=$(veb_mkveb)
	ifconfig ${veb} up
	for p in ${ep_one}a ${ep_two}a ${ep_three}a; do
		ifconfig ${p} up
		atf_check -s exit:0 -o ignore ${VEBCTL} ${veb} add ${p}
	done

	atf_check -s exit:0 -o ignore \
	    ${VEBCTL} ${veb} setflags ${ep_one}a +private
	atf_check -s exit:0 -o ignore \
	    ${VEBCTL} ${veb} setflags ${ep_two}a +private

	# private <-> private: blocked, both directions.
	atf_check -s not-exit:0 -o ignore -e ignore \
	    jexec one ping -c 1 -t 2 192.0.2.2
	atf_check -s not-exit:0 -o ignore -e ignore \
	    jexec two ping -c 1 -t 2 192.0.2.1

	# private <-> non-private: unaffected.
	atf_check -s exit:0 -o ignore jexec one ping -c 3 -t 1 192.0.2.3
	atf_check -s exit:0 -o ignore jexec two ping -c 3 -t 1 192.0.2.3
}
private_ports_cleanup()
{
	veb_cleanup
}

atf_init_test_cases()
{
	atf_add_test_case "create_destroy"
	atf_add_test_case "member_add_del"
	atf_add_test_case "add_errors"
	atf_add_test_case "bridge_mutual_exclusion"
	atf_add_test_case "ioctl_validation"
	atf_add_test_case "unprivileged"
	atf_add_test_case "flags_roundtrip"
	atf_add_test_case "vport_flag_rejected"
	atf_add_test_case "timeout_roundtrip"
	atf_add_test_case "mtu"
	atf_add_test_case "delete_with_members"
	atf_add_test_case "member_departure"
	atf_add_test_case "transmit_ipv4_unicast"
	atf_add_test_case "no_host_presence"
	atf_add_test_case "vport_host_path"
	atf_add_test_case "vport_constraints"
	atf_add_test_case "learning_expire"
	atf_add_test_case "private_ports"
}
