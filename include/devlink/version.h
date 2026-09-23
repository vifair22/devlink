#ifndef DEVLINK_VERSION_H
#define DEVLINK_VERSION_H

/*
 * Full build version, SEMVER_YYYYMMDD.HHMM.TYPE. Both the Linux client and
 * the Win98 agent embed it, and the handshake will compare them, so a stale
 * agent left on the target is caught instead of misparsing frames.
 */
const char *wl_version(void);

#endif
