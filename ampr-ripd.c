/*
 * ampr-ripd.c - AMPR 44net RIPv2 Listner Version 1.0
 *
 * Author: Marius Petrescu, YO2LOJ, <marius@yo2loj.ro>
 *
 *
 *
 * Compile with: gcc -O2 -o ampr-ripd ampr-ripd.c
 *
 *
 * Usage: ampr-ripd [-?|-h] [-d] [-v] [-s] [-r] [-i <interface>] [-a <ip>[,<ip>...]] [-p <password>] [-f <interface>] [-e <ip>]
 *
 * Options:
 *          -?, -h                usage info
 *          -d                    debug mode: no daemonization, verbose output
 *          -v                    more verbose debug output
 *          -s                    save routes to /var/lib/ampr-ripd/encap.txt (encap format),
 *                                if this file exists, it will be loaded on startup regardless
 *                                of this option
 *          -r                    use raw socket instead of multicast
 *          -i <interface>        tunnel interface to use, defaults to 'tunl0'
 *          -t <table>            routing table to use, defaults to 'main'
 *          -a  <ip>[,<ip>...]    comma separated list of IPs to be ignored
 *                                list contains local interface IPs by default
 *          -p <password>         RIPv2 password, defaults to none
 *          -f <interface>        interface for RIP forwarding, defaults to none/disabled
 *          -e <ip>               forward destination IP, defaults to 224.0.0.9 if enabled
 *
 *
 * Observation: All routes are created with protocol set to 44
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Version History
 * ---------------
 *    0.9    14.Apr.2013    Alpha release, based on Hessus's rip44d
 *    1.0     1.Aug.2013    First functional version, no tables, no tcp window setting
 *    1.1     1.Aug.2013    Fully functional version
 *    1.2     3.Aug.2013    Added option for using raw sockets instead of multicast
 *    1.3     7.Aug.2013    Minor bug fix, removed compiler warnings
 *    1.4     8.Aug.2013    Possible buffer overflow fixed
 *                          Reject metric 15 packets fixed
 *    1.5    10.Aug.2013    Corrected a stupid netmask calculation error introduced in v1.4
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <asm/types.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/route.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <time.h>
#include <ctype.h>

//#define NL_DEBUG

#define RTSIZE		1000	/* maximum number of route entries */
#define EXPTIME		600	/* route expiration in seconds */

#define RTFILE		"/var/lib/ampr-ripd/encap.txt"	/* encap file */

#define RTAB_FILE	"/etc/iproute2/rt_tables"	/* route tables */

#define	BUFFERSIZE	8192
#define MYIPSIZE	25	/* max number of local interface IPs */

#define FALSE	0
#define TRUE	!FALSE

#define RIP_HDR_LEN		4
#define RIP_ENTRY_LEN		(2+2+4*4)
#define RIP_CMD_REQUEST		1
#define RIP_CMD_RESPONSE	2
#define RIP_AUTH_PASSWD		2
#define RIP_AF_INET		2


#define RTPROT_AMPR		44

#define PERROR(s)				fprintf(stderr, "%s: %s\n", (s), strerror(errno))
#define rip_pcmd(cmd)				((cmd==1)?("Request"):((cmd==2)?("Response"):("Unknown")))
#define NLMSG_TAIL(nmsg)			((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))
#define addattr32(n, maxlen, type, data) 	addattr_len(n, maxlen, type, &data, 4)
#define rta_addattr32(rta, maxlen, type, data)	rta_addattr_len(rta, maxlen, type, &data, 4)

/* uncomment if types not available */
/*
typedef unsigned char		uint8_t;
typedef unsigned short		uint16_t;
typedef unsigned int		uint32_t;
typedef signed char		sint8_t;
typedef signed short		sint16_t;
typedef signed int		sint32_t;

*/

typedef enum
{
    ROUTE_ADD,
    ROUTE_DEL,
    ROUTE_GET
} rt_actions;

typedef struct __attribute__ ((__packed__))
{
    uint8_t command;
    uint8_t version;
    uint16_t zeros;
} rip_header;


typedef struct __attribute__ ((__packed__))
{
    uint16_t af;
    uint16_t rtag;
    uint32_t address;
    uint32_t mask;
    uint32_t nexthop;
    uint32_t metric;
} rip_entry;

typedef struct __attribute__ ((__packed__))
{
    uint16_t auth;
    uint16_t type;
    uint8_t pass[16];
} rip_auth;

typedef struct
{
    uint32_t address;
    uint32_t netmask;
    uint32_t nexthop;
    time_t timestamp;
} route_entry;


static char *usage_string = "\nAMPR RIPv2 daemon v1.5 by Marius, YO2LOJ\n\nUsage: ampr-ripd [-d] [-v] [-s] [-r] [-i <interface>] [-a <ip>[,<ip>...]] [-p <password>] [-t <table>] [-f <interface>] [-e <ip>]\n";


int debug = FALSE;
int verbose = FALSE;
int save = FALSE;
int raw = FALSE;
char *tunif = "tunl0";
unsigned int tunidx = 0;
unsigned int tunaddr;
char *ilist = NULL;
char *passwd = NULL;
char *table = NULL;
int nrtable;
char *fwif = NULL;
char *fwdest = "224.0.0.9";

int tunsd;
int fwsd;
int seq;
int updated = FALSE;

route_entry routes[RTSIZE];

uint32_t myips[MYIPSIZE];


uint32_t getip(const char *dev)
{
    struct ifreq ifr;
    int res;

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) return 0;

    memset(&ifr, 0, sizeof(struct ifreq));
    strcpy(ifr.ifr_name, dev);
    res = ioctl(sockfd, SIOCGIFADDR, &ifr);
    close(sockfd);
    if (res < 0) return 0;
    return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
}

void set_multicast(int sockfd, const char *dev)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(struct ifreq));
    strcpy(ifr.ifr_name, dev);

    if (ioctl(sockfd, SIOCGIFFLAGS, &ifr)< 0)
    {
	return;
    }

    ifr.ifr_flags |= IFF_MULTICAST;

    ioctl(sockfd, SIOCSIFFLAGS, &ifr);

    return;
}

char *ipv4_htoa(unsigned int ip)
{
    static char buf[INET_ADDRSTRLEN];
    sprintf(buf, "%d.%d.%d.%d", (ip & 0xff000000) >> 24, (ip & 0x00ff0000) >> 16, (ip & 0x0000ff00) >> 8, ip & 0x000000ff);
    return buf;
}

char *ipv4_ntoa(unsigned int ip)
{
    unsigned int lip = ntohl(ip);
    return ipv4_htoa(lip);
}

char *ipv4_ntoa_encap(int idx)
{
    static char buf[INET_ADDRSTRLEN];
    char *p;
    unsigned int lip = ntohl(routes[idx].address);
    sprintf(buf, "%d.%d", (lip & 0xff000000) >> 24, (lip & 0x00ff0000) >> 16);
    if ((((lip & 0x0000ff00) >> 8) != 0) || ((lip & 0x000000ff) != 0))
    {
	p = &buf[strlen(buf)];
	sprintf(p, ".%d", (lip & 0x0000ff00) >> 8);
	if ((lip & 0x000000ff) != 0)
	{
	    p = &buf[strlen(buf)];
	    sprintf(p, ".%d", lip & 0x000000ff);
	}
    }
    return buf;
}

void set_rt_table(char *arg)
{
    FILE *rtf;
    char buffer[255];
    char sbuffer[255];
    char *p;
    int i;

    if (NULL == arg)
    {
	nrtable =  RT_TABLE_MAIN;
	if (debug) fprintf(stderr, "Using routing table 'main' (%d).\n", nrtable);
    }
    else if (strcmp("default", arg) == 0)
    {
	nrtable =  RT_TABLE_DEFAULT;
	if (debug) fprintf(stderr, "Using routing table 'default' (%d).\n", nrtable);
    }
    else if (strcmp("main", arg) == 0)
    {
	nrtable =  RT_TABLE_MAIN;
	if (debug) fprintf(stderr, "Using routing table 'main' (%d).\n", nrtable);
    }
    else if (strcmp("local", arg) == 0)
    {
	nrtable =  RT_TABLE_LOCAL;
	if (debug) fprintf(stderr, "Using routing table 'local' (%d).\n", nrtable);
    }
    else
    {
	/* check for a number */
	for (i=0; i<strlen(arg); i++)
	{
	    if (!isdigit(arg[i]))
		break;
	}

	if (i==strlen(arg)) /* we are all digits */
	{
	    if (1 != sscanf(arg, "%d", &nrtable))
	    {
		/* fallback */
		nrtable = RT_TABLE_MAIN;
		if (debug) fprintf(stderr, "Can not find routing table '%s'. Assuming table 'main' (%d)", table, nrtable);
	    }
	    if (debug) fprintf(stderr, "Using routing table (%d).\n", nrtable);
	}
	else /* we have a table name  */
	{
	    rtf = fopen(RTAB_FILE, "r");
	    if (NULL == rtf)
	    {
		if (debug) fprintf(stderr, "Can not open routing table file '%s'. Assuming table main (254)\n", RTAB_FILE);
		nrtable = RT_TABLE_MAIN;
	    }

	    while (fgets(buffer, 255, rtf) != NULL)
	    {
		if ((buffer[0]!='#') && (p = strstr(buffer, table)) != NULL)
		{
		    if (2 == sscanf(buffer, "%d %s", &nrtable, (char *)&sbuffer))
		    {
			if (0 == strcmp(table, sbuffer))
			{
			    if (debug) fprintf(stderr, "Using routing table '%s' (%d).\n", table, nrtable);
			    return;
			}
		    }
		}
		p = NULL;
		continue;
	    }
	    nrtable = RT_TABLE_MAIN;
	    if (debug) fprintf(stderr, "Can not find routing table %s. Assuming table 'main' (%d)", table, nrtable);
	    fclose (rtf);
	}
    }
}

void detect_myips(void)
{
    int i, j;
    uint32_t ipaddr;

    struct if_nameindex *names;

    for (i=0; i<MYIPSIZE; i++) myips[i] = 0;

    names = if_nameindex();

    if (NULL == names)
    {
	return;
    }

    i = 0;
    while ((names[i].if_index != 0) && (names[i].if_name != NULL) && (i<MYIPSIZE))
    {
	ipaddr = getip(names[i].if_name);

	if (debug && verbose) fprintf(stderr, "Interface detected: %s, IP: %s\n", names[i].if_name, ipv4_ntoa(ipaddr));

	if (strcmp(names[i].if_name, tunif) == 0)
	{
	    tunidx = names[i].if_index;
	    if (debug && verbose) fprintf(stderr, "Assigned tunnel interface index: %u\n", tunidx);
	}

	/* check if address not already there */
	for (j=0; j<MYIPSIZE; j++)
	{
	    if ((myips[j] == ipaddr) || (0 == myips[j])) break;
	}
	if (MYIPSIZE != j) myips[j] = ipaddr;

	i++;
    }

    if_freenameindex(names);

    if (debug && verbose)
    {
	fprintf(stderr, "Local IPs:\n");
        for (i=0; i<MYIPSIZE; i++)
	{
	    if (0 == myips[i]) break;
	    fprintf(stderr, "   %s\n", ipv4_ntoa(myips[i]));
	}
    }
}

int check_ignore(unsigned int ip)
{
	char *ipstr;
	char *sptr;
	char *ptr;
	int i;

	/* check for a local interface match */
	for (i=0; i<MYIPSIZE; i++)
	{
	    if (0 == myips[i]) break;
	    if (ip == myips[i]) return TRUE;
	}

	/* check for a match in the ignore list */
	if (NULL != ilist)
	{
	    ipstr = ipv4_ntoa(ip);
	    sptr = ilist;

	    while ((ptr = strstr(sptr, ipstr)) != NULL)
	    {
		/* we have ip as substring in the list - have to check if it is the complete ip - has to have a comma or 0 after it */
		if ((ptr[strlen(ipstr)] == ',')||(ptr[strlen(ipstr)] == 0))
		{
			/* ip is in ignore list */
			return TRUE;
		}
		/* false alarm, continue search */
		sptr = &ptr[strlen(ipstr)];
	    }
	}
	/* the ip is valid */
	return FALSE;
};

void list_add(unsigned int address, unsigned int netmask, unsigned int nexthop)
{
    int i;

    /* find a free entry */
    for (i=0; i<RTSIZE; i++)
    {
	if (0 == routes[i].timestamp)
	{
	    routes[i].address = address;
	    routes[i].netmask = netmask;
	    routes[i].nexthop = nexthop;
	    routes[i].timestamp = time(NULL);
	    break;
	}
    }

    if (RTSIZE == i)
    {
	if (debug) fprintf(stderr, "Can not find an unused route entry.\n");
    }
}

int list_count(void)
{
    int count = 0;
    int i;

    for (i=0; i<RTSIZE; i++)
    {
	if (0 != routes[i].timestamp)
	{
	    count++;
	}
    }
    return count;
}

int list_find(unsigned int address, unsigned int netmask)
{
    int i;

    for (i=0; i<RTSIZE; i++)
    {
	if ((routes[i].address == address) && (routes[i].netmask == netmask))
	{
	    break;
	}
    }

    if (RTSIZE == i)
    {
	return -1;
    }
    else
    {
	return i;
    }
}

void list_update(unsigned int address, unsigned int netmask, unsigned int nexthop)
{
    int entry;
    entry = list_find(address, netmask);
    if (-1 != entry)
    {
	if (routes[entry].nexthop != nexthop)
	{
	    routes[entry].nexthop = nexthop;
	    updated = TRUE;
	}
	routes[entry].timestamp = time(NULL);
    }
    else
    {
	list_add(address, netmask, nexthop);
	updated = TRUE;
    }
}

void list_remove(int idx)
{
	routes[idx].address = 0;
	routes[idx].netmask = 0;
	routes[idx].nexthop = 0;
	routes[idx].timestamp = 0;

}

void list_clear(void)
{
    int i;
    for (i=0; i<RTSIZE; i++)
    {
	list_remove(i);
    }
}

void save_encap(void)
{
	int i;
	FILE *efd;
	time_t clock;

	if ((FALSE == updated) || (FALSE == save))
	{
	    if (debug && verbose) fprintf(stderr, "Saving to encap file not needed.\n");
	    return;
	}

	efd = fopen(RTFILE, "w+");
	if (NULL == efd)
	{
		if (debug) fprintf(stderr, "Can not open encap file for writing: %s\n", RTFILE);
		return;
	}

	clock = time(NULL);

	fprintf(efd, "#\n");
	fprintf(efd, "# encap.txt file - saved by ampr-ripd (UTC) %s", asctime(gmtime(&clock)));
	fprintf(efd, "#\n");

	for (i=0; i<RTSIZE; i++)
	{
		if (0 != routes[i].timestamp)
		{
		    fprintf(efd, "route addprivate %s", ipv4_ntoa_encap(i));
		    fprintf(efd, "/%d encap ", routes[i].netmask);
		    fprintf(efd, "%s\n", ipv4_ntoa(routes[i].nexthop));
		}
	}

	fprintf(efd, "# --EOF--\n");

	fclose(efd);
	
	updated = FALSE;
}

void load_encap(void)
{
	int i;
	int count = 0;
	FILE *efd;
	char buffer[255];
	char *p;
	uint32_t b1, b2, b3, b4, nr;
	uint32_t ipaddr;
	uint32_t netmask;
	uint32_t nexthop;

	efd = fopen(RTFILE, "r");
	if (NULL == efd)
	{
		if (debug) fprintf(stderr, "Can not open encap file for reading: %s\n", RTFILE);
		return;
	}

	while (fgets(buffer, 255, efd) != NULL)
	{
		if ((buffer[0]!='#') && ((p = strstr(buffer, "addprivate ")) != NULL))
		{
		    p = &p[strlen("addprivate ")];
		    b1 = b2 = b3 = b4 = 0;
		    netmask = 0;
		    ipaddr = 0;
		    nr =sscanf(p, "%d.%d.%d.%d", &b1, &b2, &b3, &b4);
		    if (nr < 2) continue;
		    ipaddr = (b1 << 24) | (b2 << 16);
		    if (nr > 2) ipaddr |= b3 << 8;
		    if (nr > 3) ipaddr |= b4;
		    p = strstr(p, "/"); p = &p[1];
		    if (sscanf(p, "%d", &netmask) != 1) continue;
		    p = strstr(p, "encap ");
		    if (p == NULL) continue;
		    p = &p[strlen("encap ")];
		    nr =sscanf(p, "%d.%d.%d.%d", &b1, &b2, &b3, &b4);
		    if (nr < 4) continue;
		    nexthop = (b1 << 24) | (b2 << 16) | b3 << 8 | b4;
		
		    /* find a free entry */
		    for (i=0; i<RTSIZE; i++)
		    {
			if (0 == routes[i].timestamp)
			{
			    routes[i].address = htonl(ipaddr);
			    routes[i].netmask = netmask;
			    routes[i].nexthop = htonl(nexthop);
			    routes[i].timestamp = 1; /* expire at first update */
			    break;
			}
		    }

		    if (RTSIZE == i)
		    {
			if (debug) fprintf(stderr, "Can not find an unused route entry.\n");
		    }

		    count++;
		}
	}

	if (debug && verbose) fprintf(stderr, "Loaded %d entries from %s\n", count, RTFILE);

	fclose(efd);
}

int addattr_len(struct nlmsghdr *n, int maxlen, int type, const void *data, int alen)
{
    int len = RTA_LENGTH(alen);
    struct rtattr *rta;

    if ((NLMSG_ALIGN(n->nlmsg_len) + len) > maxlen)
    {
	if (debug) fprintf(stderr, "Max allowed length exceeded during NLMSG assembly.\n");
	return -1;
    }
    rta = NLMSG_TAIL(n);
    rta->rta_type = type;
    rta->rta_len = len;
    memcpy(RTA_DATA(rta), data, len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + len;
    return 0;
}

int rta_addattr_len(struct rtattr *rta, int maxlen, int type, const void *data, int alen)
{
    struct rtattr *subrta;
    int len = RTA_LENGTH(alen);
    if ((RTA_ALIGN(rta->rta_len) + RTA_ALIGN(len)) > maxlen)
    {
	if (debug) fprintf(stderr, "Max allowed length exceeded during sub-RTA assembly.\n");
	return -1;
    }

    subrta = (struct rtattr *)(((void *)rta) + RTA_ALIGN(rta->rta_len));
    subrta->rta_type = type;
    subrta->rta_len = len;
    memcpy(RTA_DATA(subrta), data, alen);
    rta->rta_len = NLMSG_ALIGN(rta->rta_len) + RTA_ALIGN(len);
    return 0;
}

#ifdef NL_DEBUG
void nl_debug(void *msg, int len)
{
    struct rtattr *rtattr;
    struct nlmsghdr *rh;
    struct rtmsg *rm;
    int i;
    unsigned char *c;

    if (debug && verbose)
    {
	for (rh = (struct nlmsghdr *)msg; NLMSG_OK(rh, len); rh = NLMSG_NEXT(rh, len))
	{
	
	    if (NLMSG_ERROR == rh->nlmsg_type)
	    {
		fprintf(stderr, "NLMSG: error\n");
	    }
	    else if (NLMSG_DONE == rh->nlmsg_type)
	    {
		fprintf(stderr, "NLMSG: done\n");
	    }
	    else
	    {
		if ((RTM_NEWROUTE != rh->nlmsg_type) && (RTM_DELROUTE != rh->nlmsg_type) && (RTM_GETROUTE != rh->nlmsg_type))
		{
		    fprintf(stderr, "NLMSG: %d\n", rh->nlmsg_type);
		
		    for (i=0; i<((struct nlmsghdr *)msg)->nlmsg_len; i++)
		    {
			c = (unsigned char *)&msg;
			fprintf(stderr, "%u ", c[i]);
		    }
		    fprintf(stderr, "\n");
		}
		else
		{
		    if (RTM_NEWROUTE == rh->nlmsg_type)
		    {
			c = (unsigned char *)"request new route/route info (24)";
		    }
		    else if (RTM_DELROUTE == rh->nlmsg_type)
		    {
			c = (unsigned char *)"delete route (25)";
		    }
		    else /* RTM_GETROUTE */
		    {
			c = (unsigned char *)"get route (26)";
		    }

		    fprintf(stderr, "NLMSG: %s\n", c);
		    rm = NLMSG_DATA(rh);
		    for (rtattr = (struct rtattr *)RTM_RTA(rm); RTA_OK(rtattr, len); rtattr = RTA_NEXT(rtattr, len))
		    {
			fprintf(stderr, "RTA type: %d (%d bytes): ", rtattr->rta_type, rtattr->rta_len);
			for(i=0; i<(rtattr->rta_len - sizeof(struct rtattr)); i++)
			{
			    c = (unsigned char *)RTA_DATA(rtattr);
			    fprintf(stderr, "%u ", c[i]);
			}
			fprintf(stderr, "\n");
		    }
		}
	    }
	}
    }
}
#endif

uint32_t route_func(rt_actions action, uint32_t address, uint32_t netmask, uint32_t nexthop)
{

    int nlsd;
    int len;

    char nlrxbuf[4096];
    char mxbuf[256];

    struct {
	struct nlmsghdr hdr;
	struct rtmsg    rtm;
	char buf[1024];
    } req;

    struct rtattr *mxrta = (void *)mxbuf;

    struct sockaddr_nl sa;
    struct rtattr *rtattr;
    struct nlmsghdr *rh;
    struct rtmsg *rm;

    uint32_t result = 0;
    uint32_t window = 840;

    mxrta->rta_type = RTA_METRICS;
    mxrta->rta_len = RTA_LENGTH(0);

    memset(&req, 0, sizeof(req));

    memset(&sa, 0, sizeof(struct sockaddr_nl));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = getpid();
    sa.nl_groups = 0;

    req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.hdr.nlmsg_flags = NLM_F_REQUEST;
    req.hdr.nlmsg_seq = ++seq;
    req.hdr.nlmsg_pid = getpid();
    req.rtm.rtm_family = AF_INET;
    req.rtm.rtm_dst_len = netmask;

    if (NULL == table)
    {
        req.rtm.rtm_table = RT_TABLE_MAIN;
    }
    else
    {
        req.rtm.rtm_table = nrtable;
    }

    if (ROUTE_DEL == action)
    {
        req.rtm.rtm_protocol = RTPROT_AMPR;
        req.rtm.rtm_scope = RT_SCOPE_NOWHERE;
        req.rtm.rtm_type = RTN_UNICAST;
        req.hdr.nlmsg_type = RTM_DELROUTE;
        req.hdr.nlmsg_flags |= NLM_F_CREATE;
        result = address;
    }
    else if (ROUTE_ADD == action)
    {
	req.rtm.rtm_flags |= RTNH_F_ONLINK;
	req.rtm.rtm_protocol = RTPROT_AMPR;
	
	req.rtm.rtm_type = RTN_UNICAST;
	req.hdr.nlmsg_type = RTM_NEWROUTE;
	req.hdr.nlmsg_flags |= NLM_F_CREATE;
	result = nexthop;
    }
    else
    {
	req.hdr.nlmsg_type = RTM_GETROUTE;
	req.rtm.rtm_scope = RT_SCOPE_UNIVERSE;
    }

    addattr32(&req.hdr, sizeof(req), RTA_DST, address);

    if (ROUTE_ADD == action)
    {
	if (0 != nexthop) addattr32(&req.hdr, sizeof(req), RTA_GATEWAY, nexthop); /* gateway */
	addattr32(&req.hdr, sizeof(req), RTA_OIF, tunidx); /* dev */
	rta_addattr32(mxrta, sizeof(mxbuf), RTAX_WINDOW, window);
	addattr_len(&req.hdr, sizeof(req), RTA_METRICS, RTA_DATA(mxrta), RTA_PAYLOAD(mxrta));
    }

    if ((nlsd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE)) < 0)
    {
	if (debug) fprintf(stderr, "Can not open netlink socket.\n");
	return 0;
    }

    if (bind(nlsd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        if (debug) fprintf(stderr, "Can not bind to netlink socket.\n");
	return 0;
    }
#ifdef NL_DEBUG
    if (debug && verbose) fprintf(stderr, "NL sending request.\n");
    nl_debug(&req, req.hdr.nlmsg_len);
#endif
    if (send(nlsd, &req, req.hdr.nlmsg_len, 0) < 0)
    {
	if (debug) fprintf(stderr, "Can not talk to rtnetlink.\n");
    }

    if ((len = recv(nlsd, nlrxbuf, sizeof(nlrxbuf), MSG_DONTWAIT|MSG_PEEK)) > 0)
    {
#ifdef NL_DEBUG
	if (debug && verbose) fprintf(stderr, "NL response received.\n");
	nl_debug(nlrxbuf, len);
#endif
	if (ROUTE_GET == action)
	{
	    /* parse response for ROUTE_GET */
	    for (rh = (struct nlmsghdr *)nlrxbuf; NLMSG_OK(rh, len); rh = NLMSG_NEXT(rh, len))
	    {
		if (rh->nlmsg_type == 24) /* route info resp */
		{
		    rm = NLMSG_DATA(rh);
		    for (rtattr = (struct rtattr *)RTM_RTA(rm); RTA_OK(rtattr, len); rtattr = RTA_NEXT(rtattr, len))
		    {
			if (RTA_GATEWAY == rtattr->rta_type)
			{
			    result = *((uint32_t *)RTA_DATA(rtattr));
			}
		    }
		}
		else if (NLMSG_ERROR == rh->nlmsg_type)
		{
		    result = 0;
		}
	    }
	}
    }
    close(nlsd);
    return result;
}

void route_update(uint32_t address, uint32_t netmask, uint32_t nexthop)
{
	if (route_func(ROUTE_GET, address, netmask, 0) != nexthop)
	{
	    route_func(ROUTE_DEL, address, netmask, 0); /* fails if route does not exist - no problem */
	    if (route_func(ROUTE_ADD, address, netmask, nexthop) == 0)
	    {
		if (debug)
		{
		    fprintf(stderr, "Failed to set route %s/%d via ", ipv4_ntoa(address), netmask);
		    fprintf(stderr, "%s on dev %s. ", ipv4_ntoa(nexthop), tunif);
		}
	    }
	}
}

void route_delete_all(void)
{
	int i;

	if (debug) fprintf(stderr, "Clearing routes (%d).\n", list_count());

	for(i=0; i<RTSIZE; i++)
	{
		if (0 != routes[i].timestamp)
		{
			route_func(ROUTE_DEL, routes[i].address, routes[i].netmask, 0);
		}
	}

}

void route_set_all(void)
{
	int i;

	if (debug) fprintf(stderr, "Setting routes (%d).\n", list_count());

	for(i=0; i<RTSIZE; i++)
	{
		if (0 != routes[i].timestamp)
		{
			route_update(routes[i].address, routes[i].netmask, routes[i].nexthop);
		}
	}
}

int process_auth(char *buf, int len, int needed)
{
	rip_auth *auth = (rip_auth *)buf;

	if (needed)
	{
		if (auth->auth != 0xFFFF)
		{
			if (debug) fprintf(stderr, "Password auth requested but no password found in first RIPv2 message.\n");
			return -1;
		}
		if (ntohs(auth->type) != RIP_AUTH_PASSWD)
		{
			if (debug) fprintf(stderr, "Unsupported authentication type %d.\n", ntohs(auth->type));
			return -1;
		}

		if (strncmp((char *)auth->pass, passwd, 16) != 0)
		{
			if (debug) fprintf(stderr, "Invalid password.\n");
			return -1;
		}
	}
	else
	{
		if (auth->auth == 0xFFFF)
		{
			if (debug) fprintf(stderr, "Password found in first RIPv2 message but not set.\n");
			if (ntohs(auth->type) == RIP_AUTH_PASSWD)
			{
				if (debug) fprintf(stderr, "Simple password: %s\n", auth->pass);
			}
			return -1;
		}
	
	}
	return 0;
}

void process_entry(char *buf)
{
	rip_entry *rip = (rip_entry *)buf;

	
	if (ntohs(rip->af) != RIP_AF_INET)
	{
		if (debug && verbose) fprintf(stderr, "Unsupported address family %d.\n", ntohs(rip->af));
		return;
	}

	unsigned int mask = 1;
	unsigned int netmask = 0;
	int i;

	for (i=0; i<32; i++)
	{
	    if (rip->mask & mask)
	    {
		netmask++;
	    }
	    mask <<= 1;
	}

	if (debug && verbose)
	{
		fprintf(stderr, "Entry: address %s/%d ", ipv4_ntoa(rip->address), netmask);
		fprintf(stderr, "nexthop %s ", ipv4_ntoa(rip->nexthop));
		fprintf(stderr, "metric %d", ntohl(rip->metric));
	}

	/* drop 44.0.0.1 */
	if (rip->address == inet_addr("44.0.0.1"))
	{
	    if (debug && verbose) fprintf(stderr, " - rejected\n");
	    return;
	}

	/* validate and update the route */

	/* remove if unreachable and in list */

	if (ntohl(rip->metric) > 14)
	{
		if (debug && verbose) fprintf(stderr, " - unreacheable");
		if ((i = list_find(rip->address, netmask)) != -1)
		{
			route_func(ROUTE_DEL, rip->address, netmask, 0);
			list_remove(i);
			if (debug && verbose) fprintf(stderr, ", removed from list");
		}
		if (debug && verbose) fprintf(stderr, ".\n");
		return;
	}

	/* check if in ignore list */
	if (check_ignore(rip->nexthop))
	{
		if (debug && verbose) fprintf(stderr, " - in ignore list, rejected\n");
		return;
	}

	if (debug && verbose) fprintf(stderr, "\n");

	/* update routes */
	route_update(rip->address, netmask, rip->nexthop);
	list_update(rip->address, netmask, rip->nexthop);
}


int process_message(char *buf, int len)
{
	rip_header *hdr;

	if (len < RIP_HDR_LEN + RIP_ENTRY_LEN)
	{
		if (debug) fprintf(stderr, "RIP packet to short: %d bytes", len);
		return -1;
	}
	if (len > RIP_HDR_LEN + RIP_ENTRY_LEN * 25)
	{
		if (debug) fprintf(stderr, "RIP packet to long: %d bytes", len);
		return -1;
	}
	if ((len - RIP_HDR_LEN)%RIP_ENTRY_LEN != 0)
	{
		if (debug) fprintf(stderr, "RIP invalid packet length: %d bytes", len);
		return -1;
	}

	/* packet seems plausible, process header */

	hdr = (rip_header *)buf;

	if (debug) fprintf(stderr, "RIP len %d header version %d, Command %d (%s)\n", len, hdr->version, hdr->command, rip_pcmd(hdr->command));

	if (hdr->command != RIP_CMD_RESPONSE)
	{
		if (debug) fprintf(stderr, "Ignored non-response packet\n");
		return -1;
	}

	if (hdr->version != 2)
	{
		if (debug) fprintf(stderr, "Ignored RIP version %d packet (only accept version 2).\n", hdr->version);
		return -1;
	}

	if (hdr->zeros)
	{
		if (debug) fprintf(stderr, "Ignored packet: zero bytes are not zero.\n");
		return -1;
	}

	/* header is valid, process content */

	buf += RIP_HDR_LEN;
	len -= RIP_HDR_LEN;

	/* check password if defined */

	if (passwd != NULL)
	{
		if (-1 == process_auth(buf, len, TRUE))
		{
			return -1;
		}

		if (debug) fprintf(stderr, "Simple password authentication successful.\n");
		
		buf += RIP_ENTRY_LEN;
		len -= RIP_ENTRY_LEN;
	}
	else
	{
		if (-1 == process_auth(buf, len, FALSE))
		{
			return -1;
		}
	}

	/* simple auth ok if needed or not used */

	if (len == 0)
	{
		if (debug) fprintf(stderr, "No routing entries in this packet.\n");
		return -1;
	}

	/* we have some entries */

	if (debug) fprintf(stderr, "Processing RIPv2 packet, %d entries ", len/RIP_ENTRY_LEN);
	if (debug && verbose) fprintf(stderr, "\n");

	while (len >= RIP_ENTRY_LEN)
	{
		process_entry(buf);
		buf += RIP_ENTRY_LEN;
		len -= RIP_ENTRY_LEN;
	}

	if (debug) fprintf(stderr, "(total %d entries).\n", list_count());

	/* schedule a route expire check in 30 sec - we do this only if we have route reception */
	/* else we will keep the routes because there are no updates sources available!         */
	alarm(30);

	return 0;
}

static void on_term(int sig)
{
	if (debug && verbose) fprintf(stderr, "SIGTERM/SIGKILL received.\n");
	close(fwsd);
	close(tunsd);
	exit(0); 
}

static void on_alarm(int sig)
{
	int i;
	int count = 0;

	if (debug)
	{
		fprintf(stderr, "SIGALRM received.\n");
		fprintf(stderr, "Checking for expired routes.\n");
	}

	/* check route timestamp and remove expired routes */
	for(i=0; i<RTSIZE; i++)
	{
		if ((0 != routes[i].timestamp) && ((routes[i].timestamp + EXPTIME) < time(NULL)))
		{
			route_func(ROUTE_DEL, routes[i].address, routes[i].netmask, 0);
			list_remove(i);
			count++;
			updated = TRUE;
		}
	}

	if (debug)
	{
		fprintf(stderr, "Routes expired: %d.\n", count);
		fprintf(stderr, "Saving routes to disk.\n");
	}

	save_encap();

	if (debug) fprintf(stderr, "(total %d entries).\n", list_count());
}

static void on_hup(int sig)
{
	if (debug) fprintf(stderr, "SIGHUP received!\n");
	
	route_delete_all();
	list_clear();
}

int main(int argc, char **argv)
{
	int p;

	struct sockaddr_in sin;
	struct ip_mreq group;
	
	char databuf[BUFFERSIZE];
	char *pload;
	int len, plen;

	while ((p = getopt(argc, argv, "dvsrh?i:a:p:t:f:e:")) != -1)
	{
		switch (p)
		{
		case 'd':
			debug = TRUE;
			break;
		case 'v':
			verbose = TRUE;
			break;
		case 's':
			save = TRUE;
			break;
		case 'r':
			raw = TRUE;
			break;
		case 'i':
			tunif = optarg;
			break;
		case 'a':
			ilist = optarg;
			break;
		case 'p':
			passwd = optarg;
			break;
		case 't':
			table = optarg;
			break;
		case 'f':
			fwif = optarg;
			break;
		case 'e':
			fwdest = optarg;
			break;
		case ':':
		case 'h':
		case '?':
			fprintf(stderr, "%s", usage_string);
			return 1;
		}
	}

	set_rt_table(table);

	list_clear();
	load_encap();

	seq = time(NULL);
	
	if (debug && verbose)
	{
		fprintf(stderr, "Max list size: %d entries\n", RTSIZE);
		if (NULL !=ilist) fprintf(stderr, "Ignore list: %s\n", ilist);
	}

	tunaddr = getip(tunif);

	if (debug) fprintf(stderr, "Detected tunnel interface address: %s\n", ipv4_ntoa(tunaddr));

	detect_myips();

	route_set_all();

	if (TRUE == raw)
	{
	    /* create multicast listen socket on tunnel */

	    if (debug) fprintf(stderr, "Creating RIP UDP listening socket.\n");

	    if ((tunsd = socket(PF_INET, SOCK_RAW, 4)) < 0)
	    {
		PERROR("Raw socket");
		return 1;
	    }
	}
	else
	{
	
	    /* create multicast listen socket on tunnel */

	    if (debug) fprintf(stderr, "Creating RIP UDP listening socket.\n");

	    if ((tunsd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0)
	    {
		PERROR("Tunnel socket");
		return 1;
	    }

	    if (debug && verbose) fprintf(stderr, "Setting up multicast interface.\n");

	    int reuse = 1;
	    if (setsockopt(tunsd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0)
	    {
		PERROR("Tunnel socket: Setting SO_REUSEADDR");
		close(tunsd);
		return 1;
	    }

	    if (setsockopt(tunsd, SOL_SOCKET, SO_BINDTODEVICE, tunif, strlen(tunif)) < 0)
	    {
		PERROR("Tunnel socket: Setting SO_BINDTODEVICE");
		close(tunsd);
		return 1;
	    }

	    set_multicast(tunsd, tunif);

	    memset((char *)&sin, 0, sizeof(sin));
	    sin.sin_family = PF_INET;
	    sin.sin_addr.s_addr = INADDR_ANY; /* mandatory INADDR_ANY for multicast */
	    sin.sin_port = htons(IPPORT_ROUTESERVER);

	    if (bind(tunsd, (struct sockaddr *)&sin, sizeof(sin)))
	    {
		PERROR("Tunnel socket: Bind");
		close(tunsd);
		return 1;
	    }

	    /* disable loopback */
	    int loop = 0;
	    if (setsockopt(tunsd, IPPROTO_IP, IP_MULTICAST_LOOP, (char *)&loop, sizeof(loop)) < 0)
	    {
		PERROR("Tunnel socket: Disable loopback");
		close(tunsd);
		return 1;
	    }

	    /* join multicast group 224.0.0.9 */
	    memset((char *)&group, 0, sizeof(group));
	    group.imr_multiaddr.s_addr = inet_addr("224.0.0.9");
	    group.imr_interface.s_addr = tunaddr;

	    if (setsockopt(tunsd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&group, sizeof(group)) < 0)
	    {
		PERROR("Tunnel socket: join multicast");
		close(tunsd);
		return 1;
	    }
	}

	if (NULL != fwif)
	{
		/* create the forward socket */
		if (debug && verbose) fprintf(stderr, "Setting up forwarding interface.\n");

		if ((fwsd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0)
		{
			PERROR("Forward socket");
			close(tunsd);
			return 1;
		}

		if (setsockopt(fwsd, SOL_SOCKET, SO_BINDTODEVICE, fwif, strlen(fwif)) < 0)
		{
			PERROR("Tunnel socket: Setting SO_BINDTODEVICE");
			close(fwsd);
			close(tunsd);
			return 1;
		}

		memset((char *)&sin, 0, sizeof(sin));
		sin.sin_family = PF_INET;
		sin.sin_addr.s_addr = INADDR_ANY;
		sin.sin_port = htons(IPPORT_ROUTESERVER);
		
		if (bind(fwsd, (struct sockaddr *)&sin, sizeof(sin)))
		{
			PERROR("Forward socket: Bind");
			close(fwsd);
			close(tunsd);
			return 1;
		}
		
		memset((char *)&sin, 0, sizeof(sin));
		sin.sin_family = PF_INET;
		sin.sin_addr.s_addr = inet_addr(fwdest); 
		sin.sin_port = htons(IPPORT_ROUTESERVER);
	}

	signal(SIGTERM, on_term);
	signal(SIGKILL, on_term);
	signal(SIGHUP, on_hup);
	signal(SIGALRM, on_alarm);

	/* networking up and running */

	if (FALSE == debug)
	{
		/* try to become a daemon */
		pid_t fork_res = -1;
		fork_res = fork();
		
		if (-1 == fork_res)
		{
			PERROR("Can not become a daemon");
		}
		
		if (0 != fork_res)
		{
			/* exit parent process */
			exit(0);
		}
	}

	/* daemon or debug */

	if (debug) fprintf(stderr, "Waiting for RIPv2 broadcasts...\n");


	while (1)
	{
		if ((len = read(tunsd, databuf, BUFFERSIZE)) < 0)
		{
			if (debug) fprintf(stderr, "Socket read error.\n");
		}
		else
		{
			if (TRUE == raw)
			{
			    if (len >= 48 + (RIP_HDR_LEN + RIP_ENTRY_LEN))
			    {
				struct iphdr *iph = (struct iphdr *)(databuf + 20);
				struct udphdr *udh = (struct udphdr *)(databuf + 40);
			
				if ((iph->daddr == inet_addr("224.0.0.9")) &&
				    (iph->saddr == inet_addr("44.0.0.1")) &&
				    (udh->dest == htons(IPPORT_ROUTESERVER)) &&
				    (udh->source == htons(IPPORT_ROUTESERVER)))
				{
				    pload = &databuf[48];
				    plen = len - 48;
				}
				else
				{
				    continue;
				}
			    }
			    else
			    {
				continue;
			    }
			}
			else
			{
			    pload = databuf;
			    plen = len;
			}
			
			process_message(pload, plen);
			
			if (NULL != fwif)
			{
			    sendto(fwsd, pload, plen, 0, (struct sockaddr *)&sin, sizeof(sin));
			}
		}
	}

	/* we never reach this */
	return 0; 
}
