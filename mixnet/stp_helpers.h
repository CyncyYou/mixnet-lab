#ifndef MIXNET_STP_HELPERS_H_
#define MIXNET_STP_HELPERS_H_

#include "packet.h"

#include <stdbool.h>
#include <stdint.h>

/* Returns a monotonic timestamp in milliseconds. */
uint64_t get_now_ms(void);

/* Returns a monotonic timestamp in microseconds (finer RTT resolution). */
uint64_t get_now_us(void);

/* Allocates and initializes an STP packet. Caller owns the returned packet. */
mixnet_packet* create_stp_packet(mixnet_address root,
                                 uint16_t path_len,
                                 mixnet_address node_addr);

/* Allocates a heap copy of `src`. Caller owns the returned packet. */
mixnet_packet* clone_packet(const mixnet_packet *src);

/* Sends `pkt`, retrying until the send succeeds. Takes ownership of `pkt`. */
void safe_send(void *handle, uint8_t port, mixnet_packet *pkt);

/*
 * Returns true if bid 1 is strictly better than bid 2, where "better" means:
 * lower root address, then shorter path, then lower sender address.
 */
bool is_bid_better(mixnet_address root1, uint16_t len1, mixnet_address sender1,
                   mixnet_address root2, uint16_t len2, mixnet_address sender2);

/*
 * Returns true if the neighbor advertising (their_root, their_plen) is a
 * potential child of a node whose current bid is (my_root, my_plen): i.e.,
 * it shares the same root and sits exactly one hop further from the root.
 */
bool is_my_child(mixnet_address their_root, uint16_t their_plen,
                 mixnet_address my_root, uint16_t my_plen);

#endif // MIXNET_STP_HELPERS_H_
