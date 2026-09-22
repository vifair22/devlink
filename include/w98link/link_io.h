#ifndef W98LINK_LINK_IO_H
#define W98LINK_LINK_IO_H

#include <stdint.h>

#include "w98link/link.h"
#include "w98link/transport.h"

/*
 * Glue between the I/O-free link state machine and a real transport: one
 * call sends whatever is due, waits for input no longer than the link's next
 * timer or max_wait_ms, feeds what arrived, and sends again. Both programs'
 * main loops are built on it.
 */
wl_status wl_link_pump(struct wl_link *l, struct wl_transport *t, uint32_t max_wait_ms);

#endif
