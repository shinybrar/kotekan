/**
 * @file
 * @brief The base class for handlers which work with the McGill ICE FPGA boards
 * - iceBoardHandler : public dpdkRXhandler
 */

#ifndef ICE_BOARD_HANDLER_HPP
#define ICE_BOARD_HANDLER_HPP

#include "dpdkCore.hpp"
#include "fpga_header_functions.h"
#include "prometheusMetrics.hpp"

/**
 * @brief Abstract class which contains things which are common to processing
 *        packets from the McGill ICE FPGA boards.
 *
 * This needs to be subclassed to actualy do something with the packets, it
 * just provides a common set of functions that are needed for ICEBoard packets
 *
 * @config   alignment         Int. Align each output frame of data to this FPGA seq number edge.
 *                                  Note it could be larger than the output frame size
 *                                  (in number of FPGA samples) but must be a multiple of that.
 * @config   sample_size       Int. Default 2048. Size of a time samples (unlikely to change)
 * @config   fpga_packet_size  Int. Default 4928. Full size of the FPGA packet, including Ethernet,
 *                                                IP, UDP, and FPGA frame headers, FPGA data payload,
 *                                                FPGA footer flags, and any padding
 *                                                (but not the Ethernet CRC).
 * @config   samples_per_packet Int. Default 2.   The number of time samples per FPGA packet
 *
 * @par Metrics
 * @metric kotekan_dpdk_rx_packets_total
 *         The number of Rx packets processed since starting
 * @metric kotekan_dpdk_rx_samples_total
 *         The number of timesamples processed since starting
 *         This is basically kotekan_dpdk_rx_packets_total * samples_per_packet
 * @metric kotekan_dpdk_rx_lost_packets_total
 *         The number of lost packets since starting
 * @metric kotekan_dpdk_lost_samples_total
 *         The number of lost time smaples since starting
 * @metric kotekan_dpdk_rx_bytes_total
 *         The number of bytes processed since starting
 * @metric kotekan_dpdk_rx_errors_total
 *         The total number of all errors since starting
 *         (not including packets lost on the wire/NIC)
 * @metric kotekan_dpdk_rx_ip_cksum_errors_total
 *         The total number of IP check sum errors since starting
 * @metric kotekan_dpdk_rx_packet_len_errors_total
 *         The number of packets with incorrect lenght
 * @metric kotekan_dpdk_rx_out_of_order_errors_total
 *         The number of times we got a packet in the wrong order
 *
 * @author Andre Renard
 */
class iceBoardHandler : public dpdkRXhandler {
public:
    /// Default constructor
    iceBoardHandler(Config &config, const std::string &unique_name,
                    bufferContainer &buffer_container, int port);

    /// Same abstract function as in @c dpdkRXhandler
    virtual int handle_packet(struct rte_mbuf *mbuf) = 0;

    /// Update common stats, this should be called by subclasses implementing this function as well
    virtual void update_stats();

protected:

    /**
     * @brief Aligns the first packet.
     *
     * This function should only be used at startup to find the first packet to start processing
     *
     * Should be called by every handler.
     *
     * @param mbuf The packet to check for allignment
     * @return True if the packet is within 100 of the alignment edge,
     *         False otherwise.
     */
    bool align_first_packet(struct rte_mbuf *mbuf) {
        uint64_t seq = iceBoardHandler::get_mbuf_seq_num(mbuf);
        stream_id_t stream_id = extract_stream_id(iceBoardHandler::get_mbuf_stream_id(mbuf));

        // We allow for the fact we might miss the first packet by upto 100 FPGA frames,
        // if this happens then the missing frames at the start of the buffer frame are filled
        // in as lost packets.
        if ( ((seq % alignment) <= 100) && ((seq % alignment) >= 0 )) {

            INFO("Port %d; Got StreamID: crate: %d, slot: %d, link: %d, unused: %d",
                port, stream_id.crate_id, stream_id.slot_id, stream_id.link_id, stream_id.unused);

            last_seq = seq - seq % alignment;
            cur_seq = seq;
            port_stream_id = stream_id;
            got_first_packet = true;

            return true;
        }

        return false;
    }

    /**
     * @brief Gets the FPGA seq number from the given packet
     *
     * @param cur_mbuf The rte_mbuf containing the packet
     * @return uint64_t The FPGA seq number
     */
    inline uint64_t get_mbuf_seq_num(struct rte_mbuf * cur_mbuf) {
        return (uint64_t)(*(uint32_t *)(rte_pktmbuf_mtod(cur_mbuf, char *) + 54)) +
               (((uint64_t) (0xFFFF & (*(uint32_t *)(rte_pktmbuf_mtod(cur_mbuf, char *) + 50)))) << 32);
    }

    /**
     * @brief Gets the FPGA stream ID from the given packet
     *
     * @param cur_mbuf The rte_mbuf containing the packet
     * @return uint16_t The encoded streamID
     */
    inline uint16_t get_mbuf_stream_id(struct rte_mbuf * cur_mbuf) {
        return *(uint16_t *)(rte_pktmbuf_mtod(cur_mbuf, char *) + 44);
    }

    /**
     * @brief Checks the given packet against common errors.
     *
     * Errors include:
     * - IP Check sum failure
     * - Expected packet size
     *
     * Should be called by every handler.
     *
     * @param cur_mbuf The rte_mbuf containing the packet
     * @return True if the packet doesn't have errors and false otherwise.
     */
    inline bool check_packet(struct rte_mbuf * cur_mbuf) {
        if (unlikely((cur_mbuf->ol_flags | PKT_RX_IP_CKSUM_BAD) == 1)) {
            WARN("dpdk: Got bad packet checksum on port %d", port);
            rx_ip_cksum_errors_total += 1;
            rx_errors_total += 1;
            return false;
        }
        if (unlikely(fpga_packet_size != cur_mbuf->pkt_len)) {
            ERROR("Got packet with incorrect length: %d, expected %d",
                  cur_mbuf->pkt_len, fpga_packet_size);

            // Getting a packet with the wrong length is almost always
            // a configuration/FPGA problem that needs to be addressed.
            // So for now we just exit kotekan with an error message.
            raise(SIGINT);

            rx_packet_len_errors_total += 1;
            rx_errors_total += 1;
            return false;
        }

        // Add to common stats
        rx_packets_total += 1;
        rx_bytes_total += cur_mbuf->pkt_len;

        return true;
    }

    /**
     * @brief Checks the packet seq number hasn't gone backwards
     *
     * This check is done by looking at the @c diff value given
     * which should be the difference betwene the current FPGA seq being processed
     * and the last one seen before that.
     * @param diff The seq diff as explained above
     * @return true If the packet seq isn't older than expected, false otherwise
     */
    inline bool check_order(int64_t diff) {
        if (unlikely(diff < 0)) {
            WARN("Port: %d; Diff %" PRId64 " less than zero, duplicate, bad, or out-of-order packet; last %" PRIu64 "; cur: %" PRIu64 "",
                 port, diff, last_seq, cur_seq);
            rx_out_of_order_errors_total += 1;
            rx_errors_total += 1;
            return false;
        }
        return true;
    }

    /**
     * @brief Checks if the seq number seems like it was reset
     *
     * This would likely be the result of an FPGA reset.
     *
     * This check is done by looking at the @c diff value given
     * which should be the difference betwene the current FPGA seq being processed
     * and the last one seen before that.
     * @param diff The seq diff as explained above
     * @return true If the packet seq isn't older than expected, false otherwise
     */
    inline bool check_for_reset(int64_t diff) {
        if (unlikely(diff < -1000)) {
            ERROR("The FPGAs likely reset, kotekan stopping... (FPGA seq number was less than 1000 of highest number seen.)");
            raise(SIGINT);
            return false;
        }
        return true;
    }

    /**
     * @brief Get the difference between the current FPGA seq number and the last one seen.
     *
     * Requires the internal variables cur_seq and last_seq be set.
     *
     * @return int64_t The difference between the current FPGA seq number and the last one seen
     */
    inline int64_t get_packet_diff() {
        // Since the seq number is actually an unsigned 48-bit numdber, this cast will always be safe.
        return (int64_t)cur_seq - (int64_t)last_seq;
    }

    /// The FPAG seq number of the current packet being processed
    uint64_t cur_seq = 0;

    /// The FPGA seq number of the last packet seen (before the current one)
    uint64_t last_seq = 0;

    /// The streamID seen by this port handler
    stream_id_t port_stream_id;

    /// Set to true after the first packet is alligned.
    bool got_first_packet = false;

    /// Expected size of a time sample
    uint32_t sample_size;

    /// Expected size of an FPGA packet
    uint32_t fpga_packet_size;

    /// Expected number of time samples in each packet.
    uint32_t samples_per_packet;

    /// This is the value that we will align the first frame too.
    uint64_t alignment;

    /// Offset into the first byte of data after the Ethernet/UP/UDP/FPGA packet headers
    /// this shouldn't change, so we don't expose this to the config.
    const int32_t header_offset = 58;

    /// *** Stats (move into struct?) ***
    uint64_t rx_errors_total = 0;
    uint64_t rx_ip_cksum_errors_total = 0;
    uint64_t rx_packet_len_errors_total = 0;
    uint64_t rx_packets_total = 0;
    uint64_t rx_bytes_total = 0;
    uint64_t rx_out_of_order_errors_total = 0;
    uint64_t rx_lost_samples_total = 0;

};

inline iceBoardHandler::iceBoardHandler(Config &config, const std::string &unique_name,
                       bufferContainer &buffer_container, int port) :
    dpdkRXhandler(config, unique_name, buffer_container, port) {

    sample_size = config.get_int_default(unique_name, "sample_size", 2048);
    fpga_packet_size = config.get_int_default(unique_name, "fpga_packet_size", 4928);
    samples_per_packet = config.get_int_default(unique_name, "samples_per_packet", 2);

    alignment = config.get_int_eval(unique_name, "alignment");
}

inline void iceBoardHandler::update_stats() {
    prometheusMetrics &metrics = prometheusMetrics::instance();

    std::string tags = "port=\"" + std::to_string(port) + "\"";

    metrics.add_process_metric("kotekan_dpdk_rx_packets_total",
                                unique_name,
                                rx_packets_total,
                                tags);
    metrics.add_process_metric("kotekan_dpdk_rx_samples_total",
                                unique_name,
                                rx_packets_total * samples_per_packet,
                                tags);

    metrics.add_process_metric("kotekan_dpdk_rx_lost_packets_total",
                                unique_name,
                                (int)(rx_lost_samples_total / samples_per_packet),
                                tags);
    metrics.add_process_metric("kotekan_dpdk_lost_samples_total",
                                unique_name,
                                rx_lost_samples_total,
                                tags);

    metrics.add_process_metric("kotekan_dpdk_rx_bytes_total",
                                unique_name,
                                rx_bytes_total,
                                tags);
    metrics.add_process_metric("kotekan_dpdk_rx_errors_total",
                                unique_name,
                                rx_errors_total,
                                tags);

    metrics.add_process_metric("kotekan_dpdk_rx_ip_cksum_errors_total",
                                unique_name,
                                rx_ip_cksum_errors_total,
                                tags);
    metrics.add_process_metric("kotekan_dpdk_rx_packet_len_errors_total",
                                unique_name,
                                rx_packet_len_errors_total,
                                tags);
    metrics.add_process_metric("kotekan_dpdk_rx_out_of_order_errors_total",
                                unique_name,
                                rx_out_of_order_errors_total,
                                tags);
}

#endif