#ifndef DEVLINK_AGENT_KEYPATH_H
#define DEVLINK_AGENT_KEYPATH_H

#include <stddef.h>

/* DEVLINK.KEY beside the running program, where the agent looks for its
 * passphrase (docs/protocol.md, "Passphrase and key"). Nonzero on success. */
int agent_key_path(char *out, size_t cap);

#endif
