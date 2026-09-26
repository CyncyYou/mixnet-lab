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
#include "common/testing.h"

/**
 * RTT driver for the 8-node RING topology (Step 1 of the lab).
 *
 *          0 - 1 - 2 - 3
 *          |           |
 *          7 - 6 - 5 - 4
 *
 * In an 8-node ring the two furthest nodes are diametrically opposite: node 0
 * and node 4, four hops apart in either direction. This test-case pings from
 * node 0 to node 4. As with testcase_rtt_line, it does NOT compute the RTT
 * itself: the round-trip time is printed by the *source node* (node 0) when the
 * ping response returns to it (see the handout, Step 1, RTT: print
 * `now - send_time` on receiving a ping response). Because both timestamps are
 * taken on the same node, the measurement is unaffected by clock differences
 * between servers.
 *
 * Run it in manual mode across your cluster (co-locate the orchestrator with
 * node 0); the RTT line appears on node 0's terminal. This test-case only
 * injects the ping and confirms the round trip completed (request delivered at
 * the destination, response delivered back at the source => two pcap events).
 */
class testcase_rtt_ring final : public testcase {
public:
    explicit testcase_rtt_ring() : testcase("testcase_rtt_ring") {}

    virtual void pcap(const uint16_t /* fragment_id */,
                      const mixnet_packet *const packet) override {
        // We only expect the ping request (delivered at node 4) and the ping
        // response (delivered back at node 0).
        if (packet->type == PACKET_TYPE_PING) { pcap_count_++; }
        else { pass_pcap_ = false; }
    }

    virtual void setup() override {
        init_graph(8);
        graph_->generate_topology(graph::type::RING);
        // Default mixnet addresses are the node indices (0..7). In an 8-node
        // ring, nodes 0 and 4 are the two furthest apart (4 hops).
    }

    virtual error_code run(orchestrator& o) override {
        await_convergence();                 // let STP + routing settle
        // Watch every node's user-delivered packets via the pcap plane.
        for (uint16_t i = 0; i < graph_->num_nodes; i++) {
            DIE_ON_ERROR(o.pcap_change_subscription(i, true));
        }
        // Ping between the two furthest nodes; source = node 0.
        DIE_ON_ERROR(o.send_packet(0, 4, PACKET_TYPE_PING));
        await_packet_propagation();
        return error_code::NONE;
    }

    virtual void teardown() override {
        // Request delivered at node 4 + response delivered back at node 0.
        pass_teardown_ = (pcap_count_ == 2);
    }
};

int main(int argc, char **argv) {
    testcase_rtt_ring tc;
    return testcase::run_testcase(tc, argc, argv);
}
