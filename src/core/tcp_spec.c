#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "devlink/tcp.h"

/*
 * The line spec is parsed here rather than beside either socket layer, since
 * it is the same text on both sides and needs nothing from either.
 */

static wl_status parse_port(const char *spec, const char *p, uint16_t *port)
{
    uint32_t v = 0;

    if (*p == '\0')
        return wl_fail(WL_ERR_CORRUPT, "%s: no port after the colon", spec);
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return wl_fail(WL_ERR_CORRUPT, "%s: the port is not a number", spec);
        v = (v * 10U) + (uint32_t)(*p - '0');
        if (v > 65535U)
            return wl_fail(WL_ERR_CORRUPT, "%s: the port is above 65535", spec);
    }
    if (v == 0)
        return wl_fail(WL_ERR_CORRUPT, "%s: port 0 cannot be connected to", spec);
    *port = (uint16_t)v;
    return WL_OK;
}

wl_status wl_tcp_parse(const char *spec, char *host, size_t host_cap, uint16_t *port)
{
    const char *h;
    const char *end;
    const char *rest;
    size_t n;

    if (strncmp(spec, "tcp:", 4) != 0)
        return wl_fail(WL_ERR_INVAL, "%s is not a tcp: spec", spec);
    h = spec + 4;
    if (*h == '[') {
        h++;
        end = strchr(h, ']');
        if (end == NULL)
            return wl_fail(WL_ERR_CORRUPT, "%s: no closing bracket", spec);
        rest = end + 1;
        if (*rest != '\0' && *rest != ':')
            return wl_fail(WL_ERR_CORRUPT, "%s: expected a colon after the bracket", spec);
    } else {
        end = strchr(h, ':');
        if (end == NULL)
            end = h + strlen(h);
        else if (strchr(end + 1, ':') != NULL)
            /* Without brackets there is no telling where an IPv6 address
             * stops and the port starts. */
            return wl_fail(WL_ERR_CORRUPT, "%s: put an IPv6 address in brackets, "
                           "tcp:[ADDR]:PORT", spec);
        rest = end;
    }
    n = (size_t)(end - h);
    if (n == 0)
        return wl_fail(WL_ERR_CORRUPT, "%s: no host", spec);
    if (n + 1 > host_cap)
        return wl_fail(WL_ERR_NOSPACE, "%s: host name too long", spec);
    *port = (uint16_t)WL_TCP_DEFAULT_PORT;
    if (*rest == ':') {
        wl_status st = parse_port(spec, rest + 1, port);

        if (st != WL_OK)
            return st;
    }
    memcpy(host, h, n);
    host[n] = '\0';
    return WL_OK;
}
