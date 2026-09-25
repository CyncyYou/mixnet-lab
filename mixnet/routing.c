#include "routing.h"

#include <stdlib.h>
#include <string.h>

/*
 * Simple adjacency-list link-state database.
 *
 * Nodes are stored in a dynamically-growing array keyed by mixnet address.
 * Each node holds a list of (neighbor, cost) edges. The topology is expected
 * to be small (mixnet test networks), so linear scans are acceptable.
 */

typedef struct {
    mixnet_address neighbor;
    uint16_t       cost;
} ls_edge;

typedef struct {
    mixnet_address addr;
    ls_edge       *edges;
    uint16_t       edge_count;
    uint16_t       edge_cap;
} ls_node;

struct link_state_db {
    ls_node *nodes;
    uint16_t node_count;
    uint16_t node_cap;
};

link_state_db *lsdb_create(void) {
    link_state_db *db = (link_state_db *)calloc(1, sizeof(link_state_db));
    return db;
}

void lsdb_free(link_state_db *db) {
    if (!db) return;
    for (uint16_t i = 0; i < db->node_count; i++) {
        free(db->nodes[i].edges);
    }
    free(db->nodes);
    free(db);
}

/* Returns the node with the given address, or NULL if absent. */
static ls_node *find_node(const link_state_db *db, mixnet_address addr) {
    for (uint16_t i = 0; i < db->node_count; i++) {
        if (db->nodes[i].addr == addr) {
            return &db->nodes[i];
        }
    }
    return NULL;
}

/* Returns the node with the given address, inserting it if absent. */
static ls_node *get_or_add_node(link_state_db *db, mixnet_address addr) {
    ls_node *n = find_node(db, addr);
    if (n) return n;

    if (db->node_count == db->node_cap) {
        uint16_t new_cap = db->node_cap ? (uint16_t)(db->node_cap * 2) : 8;
        ls_node *grown = (ls_node *)realloc(db->nodes, new_cap * sizeof(ls_node));
        if (!grown) return NULL;
        db->nodes = grown;
        db->node_cap = new_cap;
    }
    n = &db->nodes[db->node_count++];
    n->addr = addr;
    n->edges = NULL;
    n->edge_count = 0;
    n->edge_cap = 0;
    return n;
}

bool lsdb_add_link(link_state_db *db, mixnet_address node,
                   mixnet_address neighbor, uint16_t cost) {
    ls_node *n = get_or_add_node(db, node);
    if (!n) return false;

    /* Update existing edge if present. */
    for (uint16_t i = 0; i < n->edge_count; i++) {
        if (n->edges[i].neighbor == neighbor) {
            if (n->edges[i].cost != cost) {
                n->edges[i].cost = cost;
                return true;
            }
            return false;
        }
    }

    /* Append a new edge. */
    if (n->edge_count == n->edge_cap) {
        uint16_t new_cap = n->edge_cap ? (uint16_t)(n->edge_cap * 2) : 4;
        ls_edge *grown = (ls_edge *)realloc(n->edges, new_cap * sizeof(ls_edge));
        if (!grown) return false;
        n->edges = grown;
        n->edge_cap = new_cap;
    }
    n->edges[n->edge_count].neighbor = neighbor;
    n->edges[n->edge_count].cost = cost;
    n->edge_count++;
    return true;
}

/*
 * Dijkstra over the address-keyed node array. We work in terms of node indices
 * within db->nodes. `dist`, `visited`, `prev`, and `first_hop` are parallel
 * arrays indexed by node index.
 *
 * `first_hop[v]` records the mixnet address of the first hop out of src on the
 * best-known path to v; this is what we use for tie-breaking (prefer smaller
 * first-hop address on equal total cost).
 */
bool lsdb_shortest_path(const link_state_db *db, mixnet_address src,
                        mixnet_address dst, mixnet_address *out_route,
                        uint16_t *out_len) {
    if (!db || db->node_count == 0) return false;

    /* Locate src and dst indices. */
    int src_idx = -1, dst_idx = -1;
    for (uint16_t i = 0; i < db->node_count; i++) {
        if (db->nodes[i].addr == src) src_idx = i;
        if (db->nodes[i].addr == dst) dst_idx = i;
    }
    if (src_idx < 0 || dst_idx < 0) return false;

    if (src == dst) {
        *out_len = 0;
        return true;
    }

    uint16_t n = db->node_count;
    uint64_t *dist = (uint64_t *)malloc(n * sizeof(uint64_t));
    bool *visited = (bool *)calloc(n, sizeof(bool));
    int *prev = (int *)malloc(n * sizeof(int));
    mixnet_address *first_hop = (mixnet_address *)malloc(n * sizeof(mixnet_address));
    if (!dist || !visited || !prev || !first_hop) {
        free(dist); free(visited); free(prev); free(first_hop);
        return false;
    }

    const uint64_t INF = UINT64_MAX;
    for (uint16_t i = 0; i < n; i++) {
        dist[i] = INF;
        prev[i] = -1;
        first_hop[i] = 0;
    }
    dist[src_idx] = 0;

    for (uint16_t iter = 0; iter < n; iter++) {
        /* Pick the unvisited node with the smallest distance; break ties on
         * smallest first-hop address for determinism. */
        int u = -1;
        for (uint16_t i = 0; i < n; i++) {
            if (visited[i] || dist[i] == INF) continue;
            if (u < 0 || dist[i] < dist[u] ||
                (dist[i] == dist[u] && first_hop[i] < first_hop[u])) {
                u = i;
            }
        }
        if (u < 0) break;
        visited[u] = true;

        ls_node *un = &db->nodes[u];
        for (uint16_t e = 0; e < un->edge_count; e++) {
            ls_node *vn = find_node(db, un->edges[e].neighbor);
            if (!vn) continue;
            int v = (int)(vn - db->nodes);
            if (visited[v]) continue;

            uint64_t nd = dist[u] + un->edges[e].cost;
            /* First hop on the path to v: if u is src, the first hop is v's
             * address itself; otherwise inherit u's first hop. */
            mixnet_address fh = (u == src_idx) ? vn->addr : first_hop[u];

            bool better = false;
            if (nd < dist[v]) {
                better = true;
            } else if (nd == dist[v]) {
                /* Equal cost: prefer the route whose first hop out of src has
                 * the smaller mixnet address. */
                if (fh < first_hop[v]) better = true;
            }
            if (better) {
                dist[v] = nd;
                prev[v] = u;
                first_hop[v] = fh;
            }
        }
    }

    bool found = (dist[dst_idx] != INF);
    if (found) {
        /* Reconstruct path from dst back to src, then strip src and dst to get
         * the intermediate hops in forward order. */
        int chain[MAX_MIXNET_ROUTE_LENGTH + 2];
        int chain_len = 0;
        int cur = dst_idx;
        while (cur != -1 && chain_len < (int)(MAX_MIXNET_ROUTE_LENGTH + 2)) {
            chain[chain_len++] = cur;
            if (cur == src_idx) break;
            cur = prev[cur];
        }
        /* chain is [dst, ..., src]; intermediates exclude both ends. */
        uint16_t hops = 0;
        for (int i = chain_len - 2; i >= 1; i--) {
            out_route[hops++] = db->nodes[chain[i]].addr;
        }
        *out_len = hops;
    }

    free(dist); free(visited); free(prev); free(first_hop);
    return found;
}

bool lsdb_random_route(const link_state_db *db, mixnet_address src,
                       mixnet_address dst, mixnet_address *out_route,
                       uint16_t *out_len) {
    if (!db || db->node_count == 0) return false;

    /* Gather candidate waypoints: any known node that is neither src nor dst. */
    mixnet_address *cands = (mixnet_address *)malloc(db->node_count *
                                                     sizeof(mixnet_address));
    if (!cands) return lsdb_shortest_path(db, src, dst, out_route, out_len);
    uint16_t ncand = 0;
    for (uint16_t i = 0; i < db->node_count; i++) {
        mixnet_address a = db->nodes[i].addr;
        if (a != src && a != dst) {
            cands[ncand++] = a;
        }
    }
    if (ncand == 0) {
        free(cands);
        return lsdb_shortest_path(db, src, dst, out_route, out_len);
    }

    /* Pick a random waypoint and stitch src->wp and wp->dst. */
    mixnet_address wp = cands[rand() % ncand];
    free(cands);

    mixnet_address leg1[MAX_MIXNET_ROUTE_LENGTH];
    mixnet_address leg2[MAX_MIXNET_ROUTE_LENGTH];
    uint16_t l1 = 0, l2 = 0;
    if (!lsdb_shortest_path(db, src, wp, leg1, &l1) ||
        !lsdb_shortest_path(db, wp, dst, leg2, &l2)) {
        return lsdb_shortest_path(db, src, dst, out_route, out_len);
    }

    /* Combined route = leg1 hops + wp + leg2 hops. */
    uint32_t total = (uint32_t)l1 + 1 + l2;
    if (total > MAX_MIXNET_ROUTE_LENGTH) {
        return lsdb_shortest_path(db, src, dst, out_route, out_len);
    }

    uint16_t k = 0;
    for (uint16_t i = 0; i < l1; i++) out_route[k++] = leg1[i];
    out_route[k++] = wp;
    for (uint16_t i = 0; i < l2; i++) out_route[k++] = leg2[i];
    *out_len = k;
    return true;
}

mixnet_packet *create_lsa_packet(mixnet_address node_address,
                                 uint16_t neighbor_count,
                                 const mixnet_address *neighbors,
                                 const uint16_t *costs) {
    uint16_t total_size = (uint16_t)(sizeof(mixnet_packet) +
                                     sizeof(mixnet_packet_lsa) +
                                     neighbor_count * sizeof(mixnet_lsa_link_params));
    mixnet_packet *pkt = (mixnet_packet *)malloc(total_size);
    if (!pkt) return NULL;
    pkt->total_size = total_size;
    pkt->type = PACKET_TYPE_LSA;

    mixnet_packet_lsa *lsa = (mixnet_packet_lsa *)pkt->payload;
    lsa->node_address = node_address;
    lsa->neighbor_count = neighbor_count;

    mixnet_lsa_link_params *links =
        (mixnet_lsa_link_params *)(pkt->payload + sizeof(mixnet_packet_lsa));
    for (uint16_t i = 0; i < neighbor_count; i++) {
        links[i].neighbor_mixaddr = neighbors[i];
        links[i].cost = costs[i];
    }
    return pkt;
}

mixnet_packet *build_data_packet(const mixnet_packet *user_pkt,
                                 mixnet_address src, mixnet_address dst,
                                 const mixnet_address *route,
                                 uint16_t route_len) {
    const uint16_t rh_hdr = sizeof(mixnet_packet_routing_header);
    /* The user packet has route_length 0, so its data begins right at the
     * `route` field, i.e. after the routing-header struct. */
    uint16_t data_size = (uint16_t)(user_pkt->total_size -
                                    sizeof(mixnet_packet) - rh_hdr);

    uint16_t route_bytes = (uint16_t)(route_len * sizeof(mixnet_address));
    uint16_t total_size = (uint16_t)(sizeof(mixnet_packet) + rh_hdr +
                                     route_bytes + data_size);

    mixnet_packet *pkt = (mixnet_packet *)malloc(total_size);
    if (!pkt) return NULL;
    pkt->total_size = total_size;
    pkt->type = PACKET_TYPE_DATA;

    mixnet_packet_routing_header *rh =
        (mixnet_packet_routing_header *)pkt->payload;
    rh->src_address = src;
    rh->dst_address = dst;
    rh->route_length = route_len;
    rh->hop_index = 0;

    mixnet_address *out_route =
        (mixnet_address *)(pkt->payload + rh_hdr);
    for (uint16_t i = 0; i < route_len; i++) {
        out_route[i] = route[i];
    }

    /* Copy the data from the user packet (located at its route field) to the
     * position immediately following the newly-installed route. */
    if (data_size > 0) {
        const char *src_data = (const char *)user_pkt->payload + rh_hdr;
        char *dst_data = (char *)(pkt->payload + rh_hdr + route_bytes);
        memcpy(dst_data, src_data, data_size);
    }
    return pkt;
}

mixnet_packet *build_ping_packet(mixnet_address src, mixnet_address dst,
                                 const mixnet_address *route,
                                 uint16_t route_len, uint64_t send_time) {
    const uint16_t rh_hdr = sizeof(mixnet_packet_routing_header);
    uint16_t route_bytes = (uint16_t)(route_len * sizeof(mixnet_address));
    uint16_t total_size = (uint16_t)(sizeof(mixnet_packet) + rh_hdr +
                                     route_bytes + sizeof(mixnet_packet_ping));

    mixnet_packet *pkt = (mixnet_packet *)malloc(total_size);
    if (!pkt) return NULL;
    pkt->total_size = total_size;
    pkt->type = PACKET_TYPE_PING;

    mixnet_packet_routing_header *rh =
        (mixnet_packet_routing_header *)pkt->payload;
    rh->src_address = src;
    rh->dst_address = dst;
    rh->route_length = route_len;
    rh->hop_index = 0;

    mixnet_address *out_route = (mixnet_address *)(pkt->payload + rh_hdr);
    for (uint16_t i = 0; i < route_len; i++) {
        out_route[i] = route[i];
    }

    mixnet_packet_ping *ping =
        (mixnet_packet_ping *)(pkt->payload + rh_hdr + route_bytes);
    ping->is_request = true;
    ping->_pad[0] = 0;
    ping->send_time = send_time;
    return pkt;
}
