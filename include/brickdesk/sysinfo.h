#ifndef BRICKDESK_SYSINFO_H
#define BRICKDESK_SYSINFO_H

#include <stdbool.h>

/*
 * Linux resource and device status sampling for the shell.
 *
 * These helpers are deliberately read-only and never invent a value.  When
 * the kernel interface is missing or unreadable, the caller receives an
 * explicit unknown result and can render that state instead of a plausible
 * default.
 */

typedef enum bd_wifi_state {
  BD_WIFI_UNKNOWN = 0,
  BD_WIFI_DISCONNECTED,
  BD_WIFI_CONNECTED
} bd_wifi_state;

/*
 * Resident memory of a process tree, in MiB.
 *
 * The tree is the root pid plus every descendant reachable through
 * /proc/<pid>/status PPid links.  VmRSS values are summed, so the reading is
 * the process-tree RSS, not a single-process RSS.  Returns -1 when the root
 * cannot be sampled or when the tree is incomplete enough that a number would
 * be misleading.
 *
 * This is a display reading only.  It is never an eviction input; the session
 * chooses the least-recently-backgrounded app.
 */
int bd_sys_process_tree_rss_mb(int pid);

/*
 * Wireless link state from <root>/sys/class/net/<iface>/wireless and
 * operstate.  Returns UNKNOWN when no wireless interface exists or the state
 * cannot be read, DISCONNECTED when an interface exists but is down, and
 * CONNECTED when at least one interface is up.
 */
bd_wifi_state bd_sys_wifi_state(const char *root);

#endif /* BRICKDESK_SYSINFO_H */
