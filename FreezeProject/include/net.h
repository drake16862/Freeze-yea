#ifndef NET_H
#define NET_H

#include <stdint.h>

struct net_device {
    const char *name;
    void (*init)(void);
    int  (*link_up)(void);
    void (*poll)(void);
};

extern struct net_device *active_net;

/* Scan PCI bus, detect and initialise the best available NIC. */
void net_scan_pci(void);

/* Call the active driver's poll handler (if any). */
void net_poll(void);

#endif /* NET_H */
