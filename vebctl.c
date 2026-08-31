/*
 * vebctl - test harness for if_veb(4).
 *
 * Usage:
 *      vebctl veb0 add tap0
 *      vebctl veb0 del tap0
 *      vebctl veb0 show                        # member list (VEBGIFS)
 *      vebctl veb0 rts                         # forwarding table (VEBGRTS)
 *      vebctl veb0 flags tap0                  # port flags (VEBGIFFLGS)
 *      vebctl veb0 setflags tap0 +sticky -discover
 *                                              # port flags (VEBSIFFLGS)
 *      vebctl veb0 timeout                     # cache timeout (VEBGTO)
 *      vebctl veb0 timeout 240                 # cache timeout (VEBSTO)
 *
 * Fault-injection switches, for exercising the driver's validation:
 *      vebctl -r 500 veb0 add tap0     # hammer an operation 500 times
 *      vebctl -m 64 veb0 rts           # cap the rts fallback allocation
 *      vebctl -c 99 veb0 add tap0      # force subcommand 99: bounds check
 *      vebctl -l 4 veb0 add tap0       # force ifd_len 4: argsize check
 *      vebctl -g veb0 add tap0         # send as SIOCGDRVSPEC: direction check
 *      vebctl -s veb0 show             # send as SIOCSDRVSPEC: direction check
 *
 * The direction of each subcommand is fixed by the driver's control table:
 * an entry carrying VC_F_COPYOUT must arrive as SIOCGDRVSPEC and one
 * without it must arrive as SIOCSDRVSPEC.  The "set" argument to do_cmd()
 * below encodes that per verb; -g/-s override it so the check itself can
 * be tested.  The table currently has 8 entries, so -c 8 is the first
 * out-of-bounds value.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_vebvar.h>

#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Fallback entry count for rts.  The driver does implement a sizing pass,
 * but a zero-length reply is ambiguous: it means either "empty table" or
 * "this kernel has no sizing pass".  When we get one we allocate this many
 * entries so the fetch pass still has somewhere to land.
 */
#define VEBCTL_RTS_FALLBACK     2048

static int               sock = -1;
static const char       *vebif;
static long              cmd_override = -1;
static long              len_override = -1;
static long              rts_max = VEBCTL_RTS_FALLBACK;
static int               dir_override = -1;     /* 0 = force GET, 1 = force SET */

/*
 * Port flags.  IFVP_VPORT is display-only: it is set by the kernel when a
 * vport(4) is added and describes what the port *is*, not how it behaves.
 * Letting userland set or clear it would make veb_enqueue() take the
 * host-delivery path on an ordinary member. veb_delete_member() would
 * then cast that member's softc to struct vport_softc.  See the note at the
 * bottom of parse_flag().
 */
static const struct {
        const char      *name;
        uint32_t         bit;
        int              settable;
} vp_flagbits[] = {
        { "learning",   IFVP_LEARNING,  1 },
        { "discover",   IFVP_DISCOVER,  1 },
        { "sticky",     IFVP_STICKY,    1 },
        { "private",    IFVP_PRIVATE,   1 },
        { "vport",      IFVP_VPORT,     0 },
};

#define NVPFLAGBITS     (sizeof(vp_flagbits) / sizeof(vp_flagbits[0]))

static void
usage(void)
{
        fprintf(stderr,
            "usage: vebctl [-gs] [-c cmd] [-l len] [-r count] [-m entries] "
            "<vebif> <verb> [args]\n"
            "\n"
            "verbs:\n"
            "   add <member>            add a member (VEBADD)\n"
            "   del <member>            remove a member (VEBDEL)\n"
            "   show                    list members (VEBGIFS)\n"
            "   rts                     dump forwarding table (VEBGRTS)\n"
            "   flags <member>          show port flags (VEBGIFFLGS)\n"
            "   setflags <member> [+-]flag ...\n"
            "                           change port flags (VEBSIFFLGS)\n"
            "   rawflags <member> <setmask> <clrmask> ...\n"
            "                           raw VEBSIFFLGS masks (testing)\n"
            "   timeout [seconds]       get or set cache timeout "
            "(VEBGTO/VEBSTO)\n"
            "\n"
            "settable flags: learning discover sticky private\n"
            "\n"
            "options:\n"
            "   -g              force SIOCGDRVSPEC\n"
            "   -s              force SIOCSDRVSPEC\n"
            "   -c cmd          override the subcommand number\n"
            "   -l len          override ifd_len\n"
            "   -r count        repeat the operation count times\n"
            "   -m entries      rts fallback allocation, in entries\n");
        exit(1);
}

static void
print_port_flags(uint32_t flags)
{
        const char *sep = "";
        size_t i;

        printf("0x%x<", flags);
        for (i = 0; i < NVPFLAGBITS; i++) {
                if ((flags & vp_flagbits[i].bit) != 0) {
                        printf("%s%s", sep, vp_flagbits[i].name);
                        sep = ",";
                        flags &= ~vp_flagbits[i].bit;
                }
        }
        if (flags != 0)
                printf("%s0x%x", sep, flags);
        printf(">");
}

static const char *
addr_type(uint8_t flags)
{

        switch (flags & IFVAF_TYPEMASK) {
        case IFVAF_DYNAMIC:
                return ("dynamic");
        case IFVAF_STATIC:
                return ("static");
        case IFVAF_STICKY:
                return ("sticky");
        default:
                return ("?");
        }
}

/*
 * Mirror of ifbridge.c's do_cmd(), minus the if_ctx plumbing.
 */
static int
do_cmd(u_long op, void *arg, size_t argsize, int set)
{
        struct ifdrv ifd;

        if (dir_override != -1)
                set = dir_override;
        if (cmd_override >= 0)
                op = (u_long)cmd_override;
        if (len_override >= 0)
                argsize = (size_t)len_override;

        memset(&ifd, 0, sizeof(ifd));
        strlcpy(ifd.ifd_name, vebif, sizeof(ifd.ifd_name));
        ifd.ifd_cmd = op;
        ifd.ifd_len = argsize;
        ifd.ifd_data = arg;

        printf("-> %s: ifd_name=%s ifd_cmd=%lu ifd_len=%zu\n",
            set ? "SIOCSDRVSPEC" : "SIOCGDRVSPEC",
            ifd.ifd_name, (unsigned long)ifd.ifd_cmd, argsize);

        return (ioctl(sock, set ? SIOCSDRVSPEC : SIOCGDRVSPEC, &ifd));
}

static int
veb_addel(u_long cmd, const char *member)
{
        struct ifvreq req;

        memset(&req, 0, sizeof(req));
        strlcpy(req.ifvr_ifsname, member, sizeof(req.ifvr_ifsname));

        /* VEBADD/VEBDEL are COPYIN|SUSER, no COPYOUT: SIOCSDRVSPEC. */
        if (do_cmd(cmd, &req, sizeof(req), 1) < 0)
                return (-1);

        printf("<- ok\n");
        return (0);
}

/*
 * VEBGIFS uses a two-pass nested-copyout protocol: call once with
 * ifvpc_len == 0 to learn the required size, then again with a buffer.
 * This requires VC_F_COPYIN on the VEBGIFS control table entry, without
 * it the kernel never sees the ifvpc_req pointer set up here.
 */
static int
veb_show(void)
{
        struct ifvpconf vpc;
        struct ifvreq *reqs = NULL;
        size_t alloclen, n, i;

        /* Pass 1: sizing. */
        memset(&vpc, 0, sizeof(vpc));
        vpc.ifvpc_len = 0;
        vpc.ifvpc_req = NULL;

        if (do_cmd(VEBGIFS, &vpc, sizeof(vpc), 0) < 0) {
                warn("VEBGIFS (sizing pass)");
                return (-1);
        }
        printf("<- sizing pass: ifvpc_len=%u (%u members)\n",
            (unsigned)vpc.ifvpc_len,
            (unsigned)(vpc.ifvpc_len / sizeof(struct ifvreq)));

        if (vpc.ifvpc_len == 0) {
                printf("%s: no members\n", vebif);
                return (0);
        }

        /*
         * Pass 2.  Over-allocate: members can be added between the two
         * calls, and the kernel truncates to whatever we asked for rather
         * than reporting the shortfall, so a silent short read is possible.
         * The slack lets us detect that case below.
         */
        alloclen = vpc.ifvpc_len + 4 * sizeof(struct ifvreq);
        if ((reqs = calloc(1, alloclen)) == NULL)
                err(1, "calloc");

        memset(&vpc, 0, sizeof(vpc));
        vpc.ifvpc_len = alloclen;
        vpc.ifvpc_req = reqs;

        if (do_cmd(VEBGIFS, &vpc, sizeof(vpc), 0) < 0) {
                warn("VEBGIFS (fetch pass)");
                free(reqs);
                return (-1);
        }

        if (vpc.ifvpc_len % sizeof(struct ifvreq) != 0)
                warnx("returned ifvpc_len=%u is not a multiple of %zu",
                    (unsigned)vpc.ifvpc_len, sizeof(struct ifvreq));

        n = vpc.ifvpc_len / sizeof(struct ifvreq);
        printf("<- fetch pass: ifvpc_len=%u (%zu members)\n",
            (unsigned)vpc.ifvpc_len, n);

        if (vpc.ifvpc_len == alloclen)
                warnx("buffer was filled exactly -- list may be truncated");

        for (i = 0; i < n; i++) {
                printf("\tmember: %-16s flags=", reqs[i].ifvr_ifsname);
                print_port_flags(reqs[i].ifvr_ifsflags);
                printf("\n");
        }

        free(reqs);
        return (0);
}

/*
 * VEBGRTS dumps the learned forwarding table.
 *
 * ifva_expire is a countdown in seconds, not an absolute time, and reads 0
 * for static and sticky entries.
 */
static int
veb_rts(void)
{
        struct ifvaconf vac;
        struct ifvareq *addrs = NULL;
        size_t alloclen, n, i;
        int sized = 0;

        /* Pass 1: sizing. */
        memset(&vac, 0, sizeof(vac));
        vac.ifvac_len = 0;
        vac.ifvac_req = NULL;

        if (do_cmd(VEBGRTS, &vac, sizeof(vac), 0) < 0) {
                warn("VEBGRTS (sizing pass)");
                return (-1);
        }

        if (vac.ifvac_len != 0) {
                sized = 1;
                alloclen = vac.ifvac_len + 8 * sizeof(struct ifvareq);
                printf("<- sizing pass: ifvac_len=%u (%u entries)\n",
                    (unsigned)vac.ifvac_len,
                    (unsigned)(vac.ifvac_len / sizeof(struct ifvareq)));
        } else {
                alloclen = (size_t)rts_max * sizeof(struct ifvareq);
                printf("<- sizing pass returned 0: either an empty table or "
                    "no sizing support; allocating %ld entries\n", rts_max);
        }

        if ((addrs = calloc(1, alloclen)) == NULL)
                err(1, "calloc");

        memset(&vac, 0, sizeof(vac));
        vac.ifvac_len = alloclen;
        vac.ifvac_req = addrs;

        if (do_cmd(VEBGRTS, &vac, sizeof(vac), 0) < 0) {
                warn("VEBGRTS (fetch pass)");
                free(addrs);
                return (-1);
        }

        if (vac.ifvac_len % sizeof(struct ifvareq) != 0)
                warnx("returned ifvac_len=%u is not a multiple of %zu",
                    (unsigned)vac.ifvac_len, sizeof(struct ifvareq));

        n = vac.ifvac_len / sizeof(struct ifvareq);
        printf("<- fetch pass: ifvac_len=%u (%zu entries)\n",
            (unsigned)vac.ifvac_len, n);

        if (vac.ifvac_len == alloclen)
                warnx("buffer was filled exactly -- table may be truncated%s",
                    sized ? "" : " (try -m with a larger value)");

        if (n == 0) {
                printf("%s: forwarding table is empty\n", vebif);
                free(addrs);
                return (0);
        }

        printf("%-17s %-16s %5s %8s  %s\n",
            "address", "member", "vlan", "expire", "type");
        for (i = 0; i < n; i++) {
                struct ifvareq *a = &addrs[i];

                printf("%-17s %-16s %5u %8lu  %s",
                    ether_ntoa((struct ether_addr *)a->ifva_dst),
                    a->ifva_ifsname,
                    (unsigned)a->ifva_vlan,
                    a->ifva_expire,
                    addr_type(a->ifva_flags));
                if ((a->ifva_flags & ~IFVAF_TYPEMASK) != 0)
                        printf(" (extra flags 0x%x)",
                            a->ifva_flags & ~IFVAF_TYPEMASK);
                printf("\n");
        }

        free(addrs);
        return (0);
}

/*
 * VEBGIFFLGS: read one port's flags.  COPYIN|COPYOUT, so SIOCGDRVSPEC.
 */
static int
veb_gifflags(const char *member)
{
        struct ifvreq req;

        memset(&req, 0, sizeof(req));
        strlcpy(req.ifvr_ifsname, member, sizeof(req.ifvr_ifsname));

        if (do_cmd(VEBGIFFLGS, &req, sizeof(req), 0) < 0) {
                warn("VEBGIFFLGS");
                return (-1);
        }

        printf("<- %s: flags=", req.ifvr_ifsname);
        print_port_flags(req.ifvr_ifsflags);
        printf("\n");
        return (0);
}

/*
 * Parse one "+flag" / "-flag" token into the set and clear masks.
 *
 * The kernel validates the combined mask against IFVPMASK and rejects
 * overlap between the two masks, so passing a bad value here is a
 * legitimate way to test veb_ioctl_sifflags().  Use -c/-l for that; this
 * parser only accepts names it knows.
 */
static int
parse_flag(const char *tok, uint32_t *setmask, uint32_t *clrmask)
{
        const char *name;
        size_t i;
        int add;

        switch (tok[0]) {
        case '+':
                add = 1;
                break;
        case '-':
                add = 0;
                break;
        default:
                warnx("flag \"%s\" must start with + or -", tok);
                return (-1);
        }
        name = tok + 1;

        for (i = 0; i < NVPFLAGBITS; i++) {
                if (strcmp(name, vp_flagbits[i].name) != 0)
                        continue;
                if (!vp_flagbits[i].settable) {
                        warnx("flag \"%s\" is not user-settable", name);
                        return (-1);
                }
                if (add)
                        *setmask |= vp_flagbits[i].bit;
                else
                        *clrmask |= vp_flagbits[i].bit;
                return (0);
        }

        warnx("unknown flag \"%s\"", name);
        return (-1);
}

/*
 * Raw VEBSIFFLGS: arbitrary set/clear masks, bypassing the flag-name
 * parser.  For negative-testing veb_ioctl_sifflags()'s IFVPUMASK and
 * overlap validation -- see the note above parse_flag().
 */
static int
veb_rawflags(const char *member, const char *setarg, const char *clrarg)
{
	struct ifvreq req;
	char *end;
	unsigned long set, clr;

	errno = 0;
	set = strtoul(setarg, &end, 0);
	if (errno != 0 || *end != '\0' || set > UINT32_MAX) {
		warnx("bad setmask \"%s\"", setarg);
		return (-1);
	}
	errno = 0;
	clr = strtoul(clrarg, &end, 0);
	if (errno != 0 || *end != '\0' || clr > UINT32_MAX) {
		warnx("bad clrmask \"%s\"", clrarg);
		return (-1);
	}

	memset(&req, 0, sizeof(req));
	strlcpy(req.ifvr_ifsname, member, sizeof(req.ifvr_ifsname));
	req.ifvr_ifssetmask = (uint32_t)set;
	req.ifvr_ifsclrmask = (uint32_t)clr;

	printf("   setmask=0x%x clrmask=0x%x\n",
	    req.ifvr_ifssetmask, req.ifvr_ifsclrmask);

	if (do_cmd(VEBSIFFLGS, &req, sizeof(req), 1) < 0) {
		warn("VEBSIFFLGS (raw)");
		return (-1);
	}

	printf("<- ok\n");
	return (veb_gifflags(member));
}

/*
 * VEBSIFFLGS: change one port's flags.  COPYIN|SUSER, no COPYOUT, so
 * SIOCSDRVSPEC.  The driver applies
 *      vp_flags = (vp_flags & ~clrmask) | setmask
 * and rejects a bit that appears in both masks.
 */
static int
veb_sifflags(const char *member, int argc, char *argv[])
{
        struct ifvreq req;
        uint32_t setmask = 0, clrmask = 0;
        int i;

        for (i = 0; i < argc; i++) {
                if (parse_flag(argv[i], &setmask, &clrmask) < 0)
                        return (-1);
        }

        memset(&req, 0, sizeof(req));
        strlcpy(req.ifvr_ifsname, member, sizeof(req.ifvr_ifsname));
        req.ifvr_ifssetmask = setmask;
        req.ifvr_ifsclrmask = clrmask;

        printf("   setmask=0x%x clrmask=0x%x\n", setmask, clrmask);

        if (do_cmd(VEBSIFFLGS, &req, sizeof(req), 1) < 0) {
                warn("VEBSIFFLGS");
                return (-1);
        }

        printf("<- ok\n");

        /* Read back so the caller can see what actually stuck. */
        return (veb_gifflags(member));
}

/*
 * VEBGTO / VEBSTO: the address cache timeout, in seconds.
 */
static int
veb_gto(void)
{
        struct ifvrparam param;

        memset(&param, 0, sizeof(param));

        if (do_cmd(VEBGTO, &param, sizeof(param), 0) < 0) {
                warn("VEBGTO");
                return (-1);
        }

        printf("<- %s: cache timeout %u seconds\n",
            vebif, (unsigned)param.ifvrp_ctime);
        return (0);
}

static int
veb_sto(const char *arg)
{
        struct ifvrparam param;
        char *end;
        unsigned long v;

        errno = 0;
        v = strtoul(arg, &end, 0);
        if (errno != 0 || *end != '\0' || v > UINT32_MAX) {
                warnx("bad timeout \"%s\"", arg);
                return (-1);
        }

        memset(&param, 0, sizeof(param));
        param.ifvrp_ctime = (uint32_t)v;

        if (do_cmd(VEBSTO, &param, sizeof(param), 1) < 0) {
                warn("VEBSTO");
                return (-1);
        }

        printf("<- ok\n");
        return (veb_gto());
}

int
main(int argc, char *argv[])
{
        const char *verb, *member = NULL, *tmoarg = NULL;
        char **flagv = NULL;
        long repeat = 1, i;
        u_long cmd = 0;
        int ch, rc = 0, flagc = 0;

        while ((ch = getopt(argc, argv, "gsc:l:r:m:")) != -1) {
                switch (ch) {
                case 'g':
                        dir_override = 0;
                        break;
                case 's':
                        dir_override = 1;
                        break;
                case 'c':
                        cmd_override = strtol(optarg, NULL, 0);
                        break;
                case 'l':
                        len_override = strtol(optarg, NULL, 0);
                        break;
                case 'r':
                        repeat = strtol(optarg, NULL, 0);
                        if (repeat < 1)
                                usage();
                        break;
                case 'm':
                        rts_max = strtol(optarg, NULL, 0);
                        if (rts_max < 1)
                                usage();
                        break;
                default:
                        usage();
                }
        }
        argc -= optind;
        argv += optind;

        if (argc < 2)
                usage();

        vebif = argv[0];
        verb = argv[1];

        if (strcmp(verb, "add") == 0 || strcmp(verb, "del") == 0) {
                cmd = (strcmp(verb, "add") == 0) ? VEBADD : VEBDEL;
                if (argc != 3)
                        usage();
                member = argv[2];
        } else if (strcmp(verb, "show") == 0 || strcmp(verb, "rts") == 0) {
                if (argc != 2)
                        usage();
        } else if (strcmp(verb, "flags") == 0) {
                if (argc != 3)
                        usage();
                member = argv[2];
        } else if (strcmp(verb, "setflags") == 0) {
                if (argc < 4)
                        usage();
                member = argv[2];
                flagv = &argv[3];
                flagc = argc - 3;
        } else if (strcmp(verb, "rawflags") == 0) {
		if (argc != 5)
			usage();
		member = argv[2];
		flagv = &argv[3]; 
        } else if (strcmp(verb, "timeout") == 0) {
                if (argc == 3)
                        tmoarg = argv[2];
                else if (argc != 2)
                        usage();
        } else
                usage();

        if ((sock = socket(AF_LOCAL, SOCK_DGRAM, 0)) < 0)
                err(1, "socket");

        for (i = 0; i < repeat; i++) {
                if (repeat > 1)
                        printf("=== iteration %ld/%ld ===\n", i + 1, repeat);

                if (strcmp(verb, "add") == 0 || strcmp(verb, "del") == 0)
                        rc = veb_addel(cmd, member);
                else if (strcmp(verb, "show") == 0)
                        rc = veb_show();
                else if (strcmp(verb, "rts") == 0)
                        rc = veb_rts();
                else if (strcmp(verb, "flags") == 0)
                        rc = veb_gifflags(member);
                else if (strcmp(verb, "setflags") == 0)
                        rc = veb_sifflags(member, flagc, flagv);
                else if (strcmp(verb, "rawflags") == 0)
			rc = veb_rawflags(member, flagv[0], flagv[1]);
                else if (tmoarg != NULL)
                        rc = veb_sto(tmoarg);
                else
                        rc = veb_gto();

                if (rc < 0) {
                	/* warn may not work how we want it */
                        if (member != NULL)
                                warn("%s %s", verb, member);
                        close(sock);
                        return (1);
                }
        }

        close(sock);
        return (0);
}
