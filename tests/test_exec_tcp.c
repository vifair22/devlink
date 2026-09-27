/*
 * The whole EXEC suite again, over the TCP link: encrypted, one connection
 * per session, the agent serving TCP alone. test_exec.c says how the two
 * runs differ.
 */
#define EXEC_OVER_TCP 1
#include "test_exec.c" /* NOLINT(bugprone-suspicious-include): the suite compiled a second way, on purpose */
