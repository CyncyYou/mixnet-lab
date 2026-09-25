/**
 * Copyright (C) 2023 Carnegie Mellon University
 *
 * This file is part of the Mixnet course project developed for
 * the Computer Networks course (15-441/641) taught at Carnegie
 * Mellon University.
 *
 * No part of the Mixnet project may be copied and/or distributed
 * without the express permission of the 15-441/641 course staff.
 */
#include "node.h"
#include "connection.h"
#include "packet.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "stp_helpers.h"
#include "stp_state.h"
#include "routing.h"

/* Returns the port index of the neighbor with address `addr`, or -1. */
static int port_for_addr(const mixnet_address *neighbor_addrs,
                         const bool *neighbor_known, uint16_t n,
                         mixnet_address addr) {
    for (uint16_t i = 0; i < n; i++) {
        if (neighbor_known[i] && neighbor_addrs[i] == addr) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Chooses a route from `src` to `dst`. When random routing is enabled and a
 * multi-node topology allows it, inserts one extra detour hop through a random
 * known node to produce a distinct (longer) path; otherwise falls back to the
 * shortest path. Returns false if no route exists.
 */
static bool choose_route(const link_state_db *db, bool random_routing,
                         mixnet_address src, mixnet_address dst,
                         mixnet_address *route, uint16_t *route_len) {
    if (random_routing) {
        return lsdb_random_route(db, src, dst, route, route_len);
    }
    return lsdb_shortest_path(db, src, dst, route, route_len);
}



void run_node(void *const handle,
              volatile bool *const keep_running,
              const struct mixnet_node_config c) {

    //init
    uint16_t n = c.num_neighbors;
    mixnet_address *neighbor_addrs = calloc(n, sizeof(mixnet_address));
    bool *neighbor_known = calloc(n, sizeof(bool));
    bool *port_is_tree = calloc(n, sizeof(bool));

    // last STP bid heard on each port (used to recompute tree ports)
    mixnet_address *nbr_root = calloc(n, sizeof(mixnet_address));
    uint16_t       *nbr_plen = calloc(n, sizeof(uint16_t));
    bool           *nbr_has_bid = calloc(n, sizeof(bool));

    for (uint16_t i = 0; i < n; i++) {
        port_is_tree[i] = true; 
    }

    mixnet_address current_root = c.node_addr;
    uint16_t path_to_root = 0;
    int root_port = -1;
    bool is_root = true;

    uint64_t now = get_now_ms();
    uint64_t last_hello_sent = now;
    uint64_t root_last_heard_ms = now; // last genuine progress toward current root

    // --- Checkpoint 2: link-state routing state ---
    link_state_db *db = lsdb_create();
    bool lsa_sent = false;              // have we flooded our own LSA yet?
    uint64_t all_neighbors_since = 0;   // when we first knew all neighbor addrs

    // Scratch route buffer for FIB lookups.
    mixnet_address route_buf[MAX_MIXNET_ROUTE_LENGTH];

    // Mixing buffer: hold non-control (DATA/PING) packets destined to be
    // forwarded/injected until we have collected `mixing_factor` of them.
    uint16_t mix_factor = c.mixing_factor ? c.mixing_factor : 1;
    mixnet_packet **mix_pkts = calloc(mix_factor, sizeof(mixnet_packet*));
    uint8_t        *mix_ports = calloc(mix_factor, sizeof(uint8_t));
    uint16_t        mix_count = 0;

    // i'm root
    for (uint16_t i = 0; i < n; i++) {
        mixnet_packet *stp_pkt = create_stp_packet(c.node_addr, 0, c.node_addr);
        safe_send(handle, i, stp_pkt);
    }

    // loop when not converge
    while (*keep_running) {
        uint8_t port = 0;
        mixnet_packet *pkt = NULL;

        // recieve packet
        if (mixnet_recv(handle, &port, &pkt) > 0) {
            
            if (pkt->type == PACKET_TYPE_STP) {
                mixnet_packet_stp *stp = (mixnet_packet_stp*) pkt->payload;

                // neighbour discover
                if (port < n && !neighbor_known[port]) {//port is the idx of this neighbor in my neighbor list
                    neighbor_addrs[port] = stp->node_address;
                    neighbor_known[port] = true;
                }

                // remember this neighbor's latest bid so we can (re)derive state
                if (port < n) {
                    nbr_root[port] = stp->root_address;
                    nbr_plen[port] = stp->path_length;
                    nbr_has_bid[port] = true;
                }

                // Recompute our best bid from scratch across ALL stored neighbor
                // bids plus our own identity. This is order-independent: whatever
                // the arrival sequence, we always converge to the true best parent.
                mixnet_address best_root = c.node_addr;
                uint16_t       best_plen = 0;
                int            best_port = -1;
                for (uint16_t i = 0; i < n; i++) {
                    if (!nbr_has_bid[i]) continue;
                    mixnet_address cand_root = nbr_root[i];
                    uint16_t       cand_plen = nbr_plen[i] + 1;
                    // A candidate parent bid is (cand_root, cand_plen) reached via
                    // neighbor addr neighbor_addrs[i]. Compare against current best,
                    // whose "sender" is our own address if we're still root, else the
                    // parent neighbor's address.
                    mixnet_address best_sender = (best_port < 0) ? c.node_addr
                                                                 : neighbor_addrs[best_port];
                    if (is_bid_better(cand_root, cand_plen, neighbor_addrs[i],
                                      best_root, best_plen, best_sender)) {
                        best_root = cand_root;
                        best_plen = cand_plen;
                        best_port = (int)i;
                    }
                }

                bool became_nonroot = (best_port >= 0);
                bool changed = (best_root != current_root) ||
                               (best_plen != path_to_root) ||
                               (best_port != root_port);

                current_root = best_root;
                path_to_root = best_plen;
                root_port    = best_port;
                is_root      = !became_nonroot;

                // Refresh the root-liveness timer only on genuine progress toward the
                // root: an advertisement received on our root port that is strictly
                // closer to the root than we are. Phantom bids circulating a cycle
                // arrive with path_length >= ours and will NOT refresh this, so a
                // dead root eventually times out (see aging block below).
                if (!is_root && port == root_port &&
                    stp->root_address == current_root &&
                    (uint16_t)(stp->path_length + 1) == path_to_root) {
                    root_last_heard_ms = get_now_ms();
                }

                // Advertise our (root, plen) to ALL neighbours (including parent) so
                // the parent recognizes us as its child and children learn our path.
                if (changed || (port == root_port)) {
                    for (uint16_t i = 0; i < n; i++) {
                        mixnet_packet *out_stp = create_stp_packet(current_root, path_to_root, c.node_addr);
                        safe_send(handle, i, out_stp);
                    }
                }

                // Recompute tree ports: parent, or a neighbor that is a potential
                // child (same root, exactly one hop further). Others are disabled.
                for (uint16_t i = 0; i < n; i++) {
                    if ((int)i == root_port) {
                        port_is_tree[i] = true;                 // parent / next hop
                    } else if (nbr_has_bid[i]) {
                        port_is_tree[i] = is_my_child(nbr_root[i], nbr_plen[i],
                                                      current_root, path_to_root);
                    } else {
                        port_is_tree[i] = true;                 // not yet heard: keep open
                    }
                }

                free(pkt);

            } else if (pkt->type == PACKET_TYPE_FLOOD) {
                if (port == n) { 
                    // from user to all ports 
                    for (uint16_t i = 0; i < n; i++) {
                        if (port_is_tree[i]) {
                            mixnet_packet *cp = clone_packet(pkt);
                            safe_send(handle, i, cp);
                        }
                    }
                    free(pkt);
                } else {
                    // from a neighbour: only accept floods that arrive on a tree
                    // port. A flood on a blocked (non-tree) port would create loops
                    // / duplicate deliveries, so drop it entirely.
                    if (port < n && port_is_tree[port]) {
                        for (uint16_t i = 0; i < n; i++) {
                            if (i != port && port_is_tree[i]) {
                                mixnet_packet *cp = clone_packet(pkt);
                                safe_send(handle, i, cp);
                            }
                        }
                        // forward to user
                        safe_send(handle, n, pkt);
                    } else {
                        free(pkt);
                    }
                }
            } else if (pkt->type == PACKET_TYPE_LSA) {
                // Absorb the LSA into our topology view, then re-flood along the
                // spanning tree (all tree ports except the one it arrived on).
                mixnet_packet_lsa *lsa = (mixnet_packet_lsa*) pkt->payload;
                mixnet_lsa_link_params *links =
                    (mixnet_lsa_link_params*)(pkt->payload + sizeof(mixnet_packet_lsa));

                bool changed = false;
                for (uint16_t i = 0; i < lsa->neighbor_count; i++) {
                    // Store both directions so the graph is undirected/consistent.
                    changed |= lsdb_add_link(db, lsa->node_address,
                                             links[i].neighbor_mixaddr, links[i].cost);
                    changed |= lsdb_add_link(db, links[i].neighbor_mixaddr,
                                             lsa->node_address, links[i].cost);
                }

                if (changed) {
                    // Re-flood only if this LSA carried new information, which
                    // guarantees termination even on topologies with loops.
                    for (uint16_t i = 0; i < n; i++) {
                        if ((int)i != (int)port && port_is_tree[i]) {
                            mixnet_packet *cp = clone_packet(pkt);
                            safe_send(handle, i, cp);
                        }
                    }
                }
                free(pkt);

            } else if (pkt->type == PACKET_TYPE_DATA ||
                       pkt->type == PACKET_TYPE_PING) {
                if (port == n) {
                    // Injected by the user: compute a route and source-route it.
                    mixnet_packet_routing_header *rh =
                        (mixnet_packet_routing_header*) pkt->payload;
                    mixnet_address dst = rh->dst_address;
                    uint16_t rlen = 0;

                    if (!choose_route(db, c.do_random_routing, c.node_addr,
                                      dst, route_buf, &rlen)) {
                        free(pkt);
                    } else {
                        mixnet_packet *outp;
                        if (pkt->type == PACKET_TYPE_DATA) {
                            outp = build_data_packet(pkt, c.node_addr, dst,
                                                     route_buf, rlen);
                        } else {
                            mixnet_packet_ping *up =
                                (mixnet_packet_ping*)(pkt->payload +
                                    sizeof(mixnet_packet_routing_header));
                            outp = build_ping_packet(c.node_addr, dst, route_buf,
                                                     rlen, up->send_time);
                        }
                        free(pkt);

                        // First hop: route[0] if any intermediate hops, else dst.
                        mixnet_address first = (rlen > 0) ? route_buf[0] : dst;
                        int p = port_for_addr(neighbor_addrs, neighbor_known, n, first);
                        if (p >= 0) {
                            // Enqueue for mixing.
                            mix_pkts[mix_count] = outp;
                            mix_ports[mix_count] = (uint8_t)p;
                            mix_count++;
                            if (mix_count >= mix_factor) {
                                for (uint16_t i = 0; i < mix_count; i++) {
                                    safe_send(handle, mix_ports[i], mix_pkts[i]);
                                }
                                mix_count = 0;
                            }
                        } else {
                            free(outp);
                        }
                    }
                } else {
                    // Received from a neighbor: we are a forwarder or the dest.
                    mixnet_packet_routing_header *rh =
                        (mixnet_packet_routing_header*) pkt->payload;

                    if (rh->dst_address == c.node_addr) {
                        // We are the destination.
                        if (pkt->type == PACKET_TYPE_PING) {
                            mixnet_address route_bytes_off =
                                (mixnet_address)(rh->route_length *
                                                 sizeof(mixnet_address));
                            mixnet_packet_ping *ping =
                                (mixnet_packet_ping*)(pkt->payload +
                                    sizeof(mixnet_packet_routing_header) +
                                    route_bytes_off);
                            if (ping->is_request) {
                                // Deliver the request to our user first.
                                mixnet_packet *user_copy = clone_packet(pkt);
                                safe_send(handle, n, user_copy);

                                // Turn the request into a response: swap src/dst,
                                // reverse the route, flip is_request, hop_index=0.
                                mixnet_address old_src = rh->src_address;
                                rh->src_address = rh->dst_address;
                                rh->dst_address = old_src;
                                mixnet_address *rt = (mixnet_address*)(pkt->payload +
                                    sizeof(mixnet_packet_routing_header));
                                for (uint16_t i = 0, j = rh->route_length - 1;
                                     i < j; i++, j--) {
                                    mixnet_address t = rt[i];
                                    rt[i] = rt[j];
                                    rt[j] = t;
                                }
                                rh->hop_index = 0;
                                ping->is_request = false;

                                mixnet_address nxt = (rh->route_length > 0)
                                    ? rt[0] : rh->dst_address;
                                int p = port_for_addr(neighbor_addrs, neighbor_known,
                                                      n, nxt);
                                if (p >= 0) {
                                    safe_send(handle, (uint8_t)p, pkt);
                                } else {
                                    free(pkt);
                                }
                            } else {
                                // Response arrived back at original sender.
                                safe_send(handle, n, pkt);
                            }
                        } else {
                            // DATA destined for us: deliver to user.
                            safe_send(handle, n, pkt);
                        }
                    } else {
                        // Forward: advance hop_index and send to the next node.
                        mixnet_address *rt = (mixnet_address*)(pkt->payload +
                            sizeof(mixnet_packet_routing_header));
                        rh->hop_index++;
                        mixnet_address nxt;
                        if (rh->hop_index < rh->route_length) {
                            nxt = rt[rh->hop_index];
                        } else {
                            nxt = rh->dst_address;
                        }
                        int p = port_for_addr(neighbor_addrs, neighbor_known, n, nxt);
                        if (p >= 0) {
                            mix_pkts[mix_count] = pkt;
                            mix_ports[mix_count] = (uint8_t)p;
                            mix_count++;
                            if (mix_count >= mix_factor) {
                                for (uint16_t i = 0; i < mix_count; i++) {
                                    safe_send(handle, mix_ports[i], mix_pkts[i]);
                                }
                                mix_count = 0;
                            }
                        } else {
                            free(pkt);
                        }
                    }
                }

            } else {
                free(pkt);
            }
        }

        now = get_now_ms();

        // periodically send hello if this is root
        if (is_root && (now - last_hello_sent >= c.root_hello_interval_ms)) {
            for (uint16_t i = 0; i < n; i++) {
                mixnet_packet *out_stp = create_stp_packet(c.node_addr, 0, c.node_addr);
                safe_send(handle, i, out_stp);
            }
            last_hello_sent = now;
        }

        // --- Checkpoint 2: originate our own LSA once we know all neighbors. ---
        // We wait until every neighbor address is known (via STP discovery) and a
        // brief settling window has passed so tree ports are stable before we
        // flood. We only send once; the network is assumed reliable in CP2.
        if (!lsa_sent) {
            bool all_known = true;
            for (uint16_t i = 0; i < n; i++) {
                if (!neighbor_known[i]) { all_known = false; break; }
            }
            if (n == 0) {
                // No neighbors: nothing to advertise or flood.
                lsa_sent = true;
            } else if (all_known) {
                if (all_neighbors_since == 0) {
                    all_neighbors_since = now;
                }
                // Let STP tree ports settle before flooding the LSA.
                if (now - all_neighbors_since >= (uint64_t)c.root_hello_interval_ms) {
                    // Seed our own links into the local topology view.
                    for (uint16_t i = 0; i < n; i++) {
                        uint16_t cost = c.link_costs ? c.link_costs[i] : 1;
                        lsdb_add_link(db, c.node_addr, neighbor_addrs[i], cost);
                        lsdb_add_link(db, neighbor_addrs[i], c.node_addr, cost);
                    }
                    // Build and flood our LSA on all tree ports.
                    uint16_t *costs = malloc(n * sizeof(uint16_t));
                    for (uint16_t i = 0; i < n; i++) {
                        costs[i] = c.link_costs ? c.link_costs[i] : 1;
                    }
                    mixnet_packet *my_lsa = create_lsa_packet(c.node_addr, n,
                                                              neighbor_addrs, costs);
                    free(costs);
                    for (uint16_t i = 0; i < n; i++) {
                        if (port_is_tree[i]) {
                            mixnet_packet *cp = clone_packet(my_lsa);
                            safe_send(handle, i, cp);
                        }
                    }
                    free(my_lsa);
                    lsa_sent = true;
                }
            }
        }


        // keeps circulating among live neighbors (with ever-growing path lengths),
        // so per-neighbor silence never triggers. Instead, we track the last time we
        // heard genuine progress toward the current root (an advertisement on our
        // root port that is strictly closer to the root). If that stalls past the
        // reelection interval, we abandon this root: purge every stored bid claiming
        // it, reset to self-root, and recompute.
        if (!is_root && (now - root_last_heard_ms >= c.reelection_interval_ms)) {
            for (uint16_t i = 0; i < n; i++) {
                if (nbr_has_bid[i] && nbr_root[i] == current_root) {
                    nbr_has_bid[i] = false;
                }
            }
            current_root = c.node_addr;
            path_to_root = 0;
            root_port = -1;
            is_root = true;
            root_last_heard_ms = now;

            for (uint16_t i = 0; i < n; i++) port_is_tree[i] = true;
            for (uint16_t i = 0; i < n; i++) {
                mixnet_packet *out_stp = create_stp_packet(c.node_addr, 0, c.node_addr);
                safe_send(handle, i, out_stp);
            }
        }

        usleep(1000); //wait
    }

    // free everything
    // Flush any packets still held in the mixing buffer so they are not leaked.
    for (uint16_t i = 0; i < mix_count; i++) {
        free(mix_pkts[i]);
    }
    free(mix_pkts);
    free(mix_ports);
    lsdb_free(db);

    free(neighbor_addrs);
    free(neighbor_known);
    free(port_is_tree);
    free(nbr_root);
    free(nbr_plen);
    free(nbr_has_bid);
}
