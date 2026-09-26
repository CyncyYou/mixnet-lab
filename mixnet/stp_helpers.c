#include "stp_helpers.h"

#include "connection.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t get_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

mixnet_packet* create_stp_packet(mixnet_address root,
                                 uint16_t path_len,
                                 mixnet_address node_addr) {
    uint16_t total_size = sizeof(mixnet_packet) + sizeof(mixnet_packet_stp);
    mixnet_packet *pkt = (mixnet_packet*) malloc(total_size);
    pkt->total_size = total_size;
    pkt->type = PACKET_TYPE_STP;

    mixnet_packet_stp *payload = (mixnet_packet_stp*) pkt->payload;
    payload->root_address = root;
    payload->path_length = path_len;
    payload->node_address = node_addr;
    return pkt;
}

mixnet_packet* clone_packet(const mixnet_packet *src) {
    mixnet_packet *copy = (mixnet_packet*) malloc(src->total_size);
    if (copy) {
        memcpy(copy, src, src->total_size);
    }
    return copy;
}

void safe_send(void *handle, uint8_t port, mixnet_packet *pkt) {
    while (mixnet_send(handle, port, pkt) == 0) {
        // retry until success
    }
}

bool is_bid_better(mixnet_address root1, uint16_t len1, mixnet_address sender1,
                   mixnet_address root2, uint16_t len2, mixnet_address sender2) {
    if (root1 != root2) return root1 < root2;
    if (len1 != len2)   return len1 < len2;
    return sender1 < sender2;
}

bool is_my_child(mixnet_address their_root, uint16_t their_plen,
                 mixnet_address my_root, uint16_t my_plen) {
    return (their_root == my_root) && (their_plen == my_plen + 1);
}
