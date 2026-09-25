#ifndef MIXNET_STP_STATE_H_
#define MIXNET_STP_STATE_H_

#include "config.h"
#include "packet.h"

#include <stdbool.h>
#include <stdint.h>

struct stp_bid {
    mixnet_address root_addr;
    uint16_t       path_len;
    mixnet_address sender_addr;
};

struct cp1_node_state {
    struct mixnet_node_config config;

    // neighbour discover
    mixnet_address *neighbor_addrs; // size = # of neighbours
    bool           *neighbor_known;

    // stp state
    mixnet_address current_root;
    uint16_t       path_to_root;
    int            root_port;       // -1 is root itself
    bool           is_root;
    bool          *port_is_tree;    // size = # of neighbours

    // timer
    uint64_t       last_hello_sent_ms;
    uint64_t       last_hello_seen_ms;
};

#endif // MIXNET_STP_STATE_H_
