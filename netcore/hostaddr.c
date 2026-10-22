/* struct ifreq and SIOCGIFADDR; kept apart from lwIP, whose TCP_MSS clashes with
 * <netinet/tcp.h> under _DEFAULT_SOURCE */
#define _DEFAULT_SOURCE 1

#include "netcore/hostaddr.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

uint32_t nc_default_route_addr(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], ifname[IFNAMSIZ];
    unsigned long dst, gw, mask;
    unsigned flags;
    int ref, use, metric, s, found = 0;
    struct ifreq ifr;
    uint32_t a = 0;

    if (!f)
        return 0;
    while (!found && fgets(line, sizeof(line), f))
        found = sscanf(line, "%15s %lx %lx %x %d %d %d %lx", ifname, &dst, &gw, &flags, &ref, &use,
                       &metric, &mask) == 8 && dst == 0 && mask == 0;
    fclose(f);
    if (!found || (s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        return 0;
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, ifname, strlen(ifname) + 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0 && ifr.ifr_addr.sa_family == AF_INET) {
        struct sockaddr_in sin;

        memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
        a = ntohl(sin.sin_addr.s_addr);
    }
    close(s);
    return a;
}
