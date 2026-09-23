#ifndef TEST_PTY_PAIR_H
#define TEST_PTY_PAIR_H

#include <pty.h>
#include <unistd.h>

#include "devlink/posix_fd.h"
#include "devlink/transport.h"

/* A pty pair stands in for a null-modem cable: the slave side is opened as a
 * serial port through wl_serial_open (so the termios setup is exercised),
 * the master side is a plain descriptor transport. */
static inline void pty_pair(struct wl_transport *serial_side, struct wl_transport *peer)
{
    int master;
    int slave;
    char name[128];

    assert_int_equal(openpty(&master, &slave, name, NULL, NULL), 0);
    assert_int_equal(wl_serial_open(name, 115200, serial_side), WL_OK);
    /* wl_serial_open opened its own descriptor; this one only kept the pty
     * alive until then. */
    (void)close(slave);
    assert_int_equal(wl_fd_transport(master, peer), WL_OK);
}

#endif
