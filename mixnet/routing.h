#ifndef MIXNET_ROUTING_H_
#define MIXNET_ROUTING_H_

#include "packet.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Link-state routing module.
 *
 * Maintains a global view of the mixnet topology (built from received LSAs)
 * and computes shortest paths via Dijkstra's algorithm. Routes are returned
 * excluding both the source and destination node (i.e., the intermediate
 * hops that populate a Routing Header's `route` field).
 */

/* Opaque link-state database. */
typedef struct link_state_db link_state_db;

/* Creates an empty link-state database owned by the caller. */
link_state_db *lsdb_create(void);

/* Frees a link-state database. */
void lsdb_free(link_state_db *db);

/*
 * Records that `node` has a directed link to `neighbor` with the given cost.
 * Links are stored per (node, neighbor) pair; repeated calls update the cost.
 * Returns true if this introduced new information (a new edge or changed cost).
 */
bool lsdb_add_link(link_state_db *db, mixnet_address node,
                   mixnet_address neighbor, uint16_t cost);

/*
 * Computes the shortest path from `src` to `dst` using Dijkstra's algorithm.
 *
 * On success, writes the intermediate hops (excluding src and dst) into
 * `out_route` (which must hold at least MAX_MIXNET_ROUTE_LENGTH entries) and
 * stores the number of hops in `out_len`. Returns true if a path exists.
 *
 * Ties on total cost are broken by preferring the path whose next hop out of
 * `src` has the smallest mixnet address.
 */
bool lsdb_shortest_path(const link_state_db *db, mixnet_address src,
                        mixnet_address dst, mixnet_address *out_route,
                        uint16_t *out_len);

/*
 * Computes a (possibly non-shortest) route from `src` to `dst` by routing
 * through a randomly chosen waypoint node, concatenating the shortest paths
 * src->waypoint and waypoint->dst. The waypoint itself becomes an explicit hop.
 * Falls back to the plain shortest path when no suitable waypoint exists or the
 * combined route would exceed MAX_MIXNET_ROUTE_LENGTH. Intermediate hops
 * (excluding src and dst) are written to `out_route`; count in `out_len`.
 * Returns true if any route exists.
 */
bool lsdb_random_route(const link_state_db *db, mixnet_address src,
                       mixnet_address dst, mixnet_address *out_route,
                       uint16_t *out_len);

/*
 * Allocates an LSA packet advertising `node_address` and its `neighbor_count`
 * neighbors. `neighbors` and `costs` are parallel arrays of length
 * `neighbor_count`. Caller owns the returned packet.
 */
mixnet_packet *create_lsa_packet(mixnet_address node_address,
                                 uint16_t neighbor_count,
                                 const mixnet_address *neighbors,
                                 const uint16_t *costs);

/*
 * Given a user-provided DATA packet received on the input port (route_length
 * == 0, data sitting at the `route` field), rebuilds it as a fully source-
 * routed packet: sets src/dst, installs the `route` (intermediate hops), sets
 * route_length and hop_index=0, and relocates the data payload after the route.
 *
 * Returns a newly allocated packet the caller owns; the original is untouched.
 * `route`/`route_len` describe the intermediate hops (excluding src and dst).
 */
mixnet_packet *build_data_packet(const mixnet_packet *user_pkt,
                                 mixnet_address src, mixnet_address dst,
                                 const mixnet_address *route,
                                 uint16_t route_len);

/*
 * Builds a source-routed PING request packet from a user-provided PING packet.
 * Sets the routing header and (re)writes the ping payload (is_request=true,
 * send_time). Returns a newly allocated packet the caller owns.
 */
mixnet_packet *build_ping_packet(mixnet_address src, mixnet_address dst,
                                 const mixnet_address *route,
                                 uint16_t route_len, uint64_t send_time);

#endif // MIXNET_ROUTING_H_
