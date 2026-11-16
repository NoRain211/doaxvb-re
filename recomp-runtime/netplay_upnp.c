#include "netplay.h"

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include <stdio.h>
#include <string.h>

static struct UPNPUrls urls;
static struct IGDdatas data;
static char mapped_port[6];
static char mapped_client[16];
static const char forward_description[] = "DOAXBV netplay";

/* Sets *ours when the router forwards UDP port to client under this game's description. */
static int look_up_forward(const char *port, const char *client, bool *ours)
{
    char in_client[16] = "", in_port[6] = "", desc[80] = "", enabled[4] = "", lease[16] = "";
    const int result = UPNP_GetSpecificPortMappingEntry(urls.controlURL, data.first.servicetype, port, "UDP",
        NULL, in_client, in_port, desc, enabled, lease);
    *ours = result == UPNPCOMMAND_SUCCESS && strcmp(in_client, client) == 0
        && strcmp(desc, forward_description) == 0;
    return result;
}

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
            "forward UDP port %u to this computer by hand\n", port);
        return false;
    }
    snprintf(text, sizeof text, "%u", port);
    /* Routers that overwrite instead of refusing would take another device's or program's forward. */
    bool ours;
    if (look_up_forward(text, lan, &ours) == UPNPCOMMAND_SUCCESS && !ours) {
        FreeUPNPUrls(&urls);
        fprintf(stderr, "recomp netplay: the router already forwards UDP port %u to another device or "
            "program; forward it to this computer by hand\n", port);
        return false;
    }
    /* A day's lease outlasts any session, and a crash's forward expires on its own. Routers
       that take only permanent forwards answer 725 (OnlyPermanentLeasesSupported). */
    int result = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, text, text, lan,
        forward_description, "UDP", NULL, "86400");
    if (result == 725)
        result = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, text, text, lan,
            forward_description, "UDP", NULL, "0");
    if (result != UPNPCOMMAND_SUCCESS) {
        FreeUPNPUrls(&urls);
        fprintf(stderr, "recomp netplay: the router refused to forward UDP port %u (%s); "
            "forward it to this computer by hand\n", port, strupnperror(result));
        return false;
    }
    snprintf(mapped_port, sizeof mapped_port, "%s", text);
    snprintf(mapped_client, sizeof mapped_client, "%s", lan);
    if (found == UPNP_PRIVATEIP_IGD)
        fprintf(stderr, "recomp netplay: the router forwards UDP port %u, but its own address %s is "
            "private, so players outside your provider's network cannot reach it\n", port, wan);
    else {
        fprintf(stderr, "recomp netplay: the router forwards UDP port %u; the other player joins %s:%u\n",
            port, wan, port);
        if (size != 0u) snprintf(address, size, "%s:%u", wan, port);
    }
    return true;
}

void recomp_netplay_close_router_port(void)
{
    if (mapped_port[0] == '\0') return;
    /* The forward may have been replaced since we added it. Keep it only when the router says
       it is someone else's: a failed lookup still deletes, so a permanent forward never stays. */
    bool ours;
    int result = look_up_forward(mapped_port, mapped_client, &ours);
    const bool deleting = ours || (result != UPNPCOMMAND_SUCCESS && result != 714);
    if (deleting)
        result = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, mapped_port, "UDP", NULL);
    if (result == UPNPCOMMAND_SUCCESS && !deleting)
        fprintf(stderr, "recomp netplay: left the router's forward of UDP port %s in place; it now belongs "
            "to another device or program\n", mapped_port);
    else if (result != UPNPCOMMAND_SUCCESS && result != 714) /* 714: NoSuchEntryInArray, already gone */
        fprintf(stderr, "recomp netplay: could not remove the router's forward of UDP port %s (%s)\n",
            mapped_port, strupnperror(result));
    FreeUPNPUrls(&urls);
    mapped_port[0] = '\0';
}
