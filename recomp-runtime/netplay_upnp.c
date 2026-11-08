#include "netplay.h"

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include <stdio.h>

static struct UPNPUrls urls;
static struct IGDdatas data;
static char mapped_port[6];

bool recomp_netplay_open_router_port(uint16_t port, char *address, size_t size)
{
    int error = 0;
    char route[64] = "", lan[64] = "", wan[64] = "", text[6];
    if (size != 0u) address[0] = '\0';
    /* VPN, WSL and Hyper-V adapters can take the default multicast route,
       and the router never hears the search. */
    const bool routed = recomp_netplay_route_address(route, sizeof route);
    struct UPNPDev *devices = upnpDiscover(2000, routed ? route : NULL, NULL, UPNP_LOCAL_PORT_ANY, 0, 2, &error);
    const int found = devices != NULL
        ? UPNP_GetValidIGD(devices, &urls, &data, lan, sizeof lan, wan, sizeof wan) : UPNP_NO_IGD;
    freeUPNPDevlist(devices);
    if (found != UPNP_CONNECTED_IGD && found != UPNP_PRIVATEIP_IGD) {
        if (found != UPNP_NO_IGD) FreeUPNPUrls(&urls);
        fprintf(stderr, "recomp netplay: no router answered UPnP; if the other player cannot join, "
            "forward UDP port %u to this computer by hand\n", (unsigned)port);
        return false;
    }
    snprintf(text, sizeof text, "%u", (unsigned)port);
    /* A day's lease outlasts any session, and a crash's forward expires on its own. Routers
       that take only permanent forwards answer 725 (OnlyPermanentLeasesSupported). */
    int result = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, text, text, lan,
        "DOAXBV netplay", "UDP", NULL, "86400");
    if (result == 725)
        result = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, text, text, lan,
            "DOAXBV netplay", "UDP", NULL, "0");
    if (result != UPNPCOMMAND_SUCCESS) {
        FreeUPNPUrls(&urls);
        fprintf(stderr, "recomp netplay: the router refused to forward UDP port %u (%s); "
            "forward it to this computer by hand\n", (unsigned)port, strupnperror(result));
        return false;
    }
    snprintf(mapped_port, sizeof mapped_port, "%s", text);
    if (found == UPNP_PRIVATEIP_IGD)
        fprintf(stderr, "recomp netplay: the router forwards UDP port %u, but its own address %s is "
            "private, so players outside your provider's network cannot reach it\n", (unsigned)port, wan);
    else {
        fprintf(stderr, "recomp netplay: the router forwards UDP port %u; the other player joins %s:%u\n",
            (unsigned)port, wan, (unsigned)port);
        if (size != 0u) snprintf(address, size, "%s:%u", wan, (unsigned)port);
    }
    return true;
}

void recomp_netplay_close_router_port(void)
{
    if (mapped_port[0] == '\0') return;
    const int result = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, mapped_port, "UDP", NULL);
    if (result != UPNPCOMMAND_SUCCESS)
        fprintf(stderr, "recomp netplay: could not remove the router's forward of UDP port %s (%s)\n",
            mapped_port, strupnperror(result));
    FreeUPNPUrls(&urls);
    mapped_port[0] = '\0';
}
