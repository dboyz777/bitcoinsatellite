// Copyright (c) 2016, 2017 Matt Corallo
// Unlike the rest of Bitcoin Core, this file is
// distributed under the Affero General Public License (AGPL v3)

#ifndef BITCOIN_UDPNET_H
#define BITCOIN_UDPNET_H

#include <atomic>
#include <stdint.h>
#include <vector>
#include <mutex>
#include <assert.h>
#include <chain.h>

#include <udpapi.h>
#include <netaddress.h>

#include <blockencodings.h>
#include <fec.h>

// This is largely the API between udpnet and udprelay, see udpapi for the
// external-facing API

// Local stuff only uses magic, net stuff only uses protocol_version,
// so both need to be changed any time wire format changes
static const unsigned char LOCAL_MAGIC_BYTES[] = { 0xab, 0xad, 0xca, 0xfe };
static const uint32_t UDP_PROTOCOL_VERSION = (4 << 16) | 4; // Min version 3, current version 3

enum UDPMessageType {
    MSG_TYPE_SYN = 0,
    MSG_TYPE_KEEPALIVE = 1, // aka SYN_ACK
    MSG_TYPE_DISCONNECT = 2,
    MSG_TYPE_BLOCK_HEADER = 3,
    MSG_TYPE_BLOCK_CONTENTS = 4,
    MSG_TYPE_PING = 5,
    MSG_TYPE_PONG = 6,
    MSG_TYPE_TX_CONTENTS = 7,
};

static const uint8_t UDP_MSG_TYPE_FLAGS_MASK = 0b11100000;
static const uint8_t UDP_MSG_TYPE_TYPE_MASK = 0b00011111;

struct __attribute__((packed)) UDPMessageHeader {
    uint64_t chk1;
    uint64_t chk2;
    uint8_t msg_type; // A UDPMessageType + flags
};
static_assert(sizeof(UDPMessageHeader) == 17, "__attribute__((packed)) must work");

// Message body cannot exceed 1167 bytes (1185 bytes in total UDP message contents, with a padding byte in message)
// Local send logic assumes this to be the size of block data packets in a few places!
#define MAX_UDP_MESSAGE_LENGTH 1167

enum UDPBlockMessageFlags { // Put in the msg_type
    EMPTY_BLOCK = (1 << 5), // mark when block body is empty (only header is sent)
    HAVE_BLOCK = (1 << 6),
    TIP_BLOCK  = (1 << 7)  // mark that this is a block on the chain's tip (relayed)
};

struct __attribute__((packed)) UDPBlockMessage { // (also used for txn)
    /**
     * First 8 bytes of blockhash, interpreted in LE (note that this will not include 0s, those are at the end).
     * For txn, first 8 bytes of tx, though this should change in the future.
     * Neither block nor tx recv-side logic cares what this is as long as it mostly-uniquely identifies the
     * object being sent!
     */
    uint64_t hash_prefix;
    uint32_t obj_length; // Size of full FEC-coded data
    uint32_t chunk_id : 24;
    unsigned char data[FEC_CHUNK_SIZE];
};
static_assert(sizeof(UDPBlockMessage) == MAX_UDP_MESSAGE_LENGTH, "Messages must be == MAX_UDP_MESSAGE_LENGTH");
static const size_t udp_blk_msg_header_size = sizeof(UDPBlockMessage) - FEC_CHUNK_SIZE;

struct __attribute__((packed)) UDPMessage {
    UDPMessageHeader header;
    union __attribute__((packed)) {
        unsigned char message[MAX_UDP_MESSAGE_LENGTH + 1];
        uint64_t longint;
        struct UDPBlockMessage block;
    } msg;
};
static_assert(sizeof(UDPMessage) == 1185, "__attribute__((packed)) must work");
#define PACKET_SIZE (sizeof(UDPMessage) + 40 + 8)
static_assert(PACKET_SIZE <= 1280, "All packets must fit in min-MTU for IPv6");
static_assert(sizeof(UDPMessage) == sizeof(UDPMessageHeader) + MAX_UDP_MESSAGE_LENGTH + 1, "UDPMessage should have 1 padding byte");

enum UDPState {
    STATE_INIT = 0, // Indicating the node was just added
    STATE_GOT_SYN = 1, // We received their SYN
    STATE_GOT_SYN_ACK = 1 << 1, // We've received a KEEPALIVE (which they only send after receiving our SYN)
    STATE_INIT_COMPLETE = STATE_GOT_SYN | STATE_GOT_SYN_ACK, // We can now send data to this peer
};

struct BlockChunkCount {
    uint32_t data_rcvd = 0;   // number of received block contents (data) chunks
    uint32_t data_used = 0;   // number of used block contents (data) chunks
    uint32_t header_rcvd = 0; // number of received block header chunks
    uint32_t header_used = 0; // number of used block header chunks
    uint32_t data_to_decode = 0;   // number of data chunks until able to decode
    uint32_t header_to_decode = 0; // number of header chunks until able to decode
    std::chrono::steady_clock::time_point t_first = std::chrono::steady_clock::now();  // time first chunk received
    std::chrono::steady_clock::time_point t_decode = std::chrono::steady_clock::now(); // time when ready to decode
    std::chrono::steady_clock::time_point t_last = std::chrono::steady_clock::now();   // last time chunk received
    double accum_chunk_interval = 0; // accumulator for average interval computation
    double avg_chunk_interval = 0;   // average interval between chunks
    /* NOTE: not all chunks received before the block becomes decodable are
     * effectively used. Some chunks can be known already, in which case they
     * are dropped (not used). Thus, not necessarily the count of
     * `data_to_decode` ends up being equal to `data_used`, although often it
     * will be, especially if the sender always sends random (non repeated)
     * chunk ids. The same holds for `header_to_decode` and `header_used`. */
};

struct PartialBlockData {
    const std::chrono::steady_clock::time_point timeHeaderRecvd;
    const CService nodeHeaderRecvd;

    std::atomic_bool in_header; // Indicates we are currently downloading header (or block txn)
    std::atomic_bool blk_initialized; // Indicates Init has been called with a block contents message
    std::atomic_bool header_initialized; // Indicates Init has been called with a block header message
    std::atomic_bool is_decodeable; // Indicates decoder.DecodeReady() && !in_header
    std::atomic_bool is_header_processing; // Indicates in_header && !initialized but header is ready
    std::atomic_bool packet_awaiting_lock; // Indicates there is a packet ready to process that needs state_mutex
    std::atomic_bool awaiting_processing; // Indicates the block has been pushed to the processing queue already

    std::mutex state_mutex;
    // Background thread is preparing to, and is submitting to core
    // This is set with state_mutex held, and afterwards block_data and
    // perNodeChunkCount should be treated read-only.
    std::atomic_bool currentlyProcessing;

    uint32_t blk_len; // length of chunk-coded block being downloaded
    uint32_t header_len; // length of CBlockHeaderAndLengthShortTxIDs (aka "block header") being downloaded
    std::vector<unsigned char> header_data; // Serialized block header (CBlockHeaderAndLengthShortTxIDs)
    FECDecoder header_decoder; // Note that this may have been std::move()d if (currentlyProcessing)
    FECDecoder body_decoder; // Note that this may have been std::move()d if (currentlyProcessing)
    PartiallyDownloadedChunkBlock block_data;
    bool tip_blk; // Whether this is a block at the tip of the chain or an old/repeated block

    // nodes with chunks_avail set -> packets that were useful, packets provided
    std::map<CService, std::pair<uint32_t, uint32_t>> perNodeChunkCount;

    bool Init(const UDPMessage& msg);
    ReadStatus ProvideHeaderData(const CBlockHeaderAndLengthShortTxIDs& header);
    PartialBlockData(const CService& node, const UDPMessage& header_msg, const std::chrono::steady_clock::time_point& packet_recv); // Must be a MSG_TYPE_BLOCK_HEADER
    void ReconstructBlockFromDecoder();
};

class ChunksAvailableSet {
private:
    bool allSent;
    mutable bool header_tracker_initd;
    mutable bool block_tracker_initd;
    mutable BlockChunkRecvdTracker header_tracker;
    mutable BlockChunkRecvdTracker block_tracker;

    void InitTracker(size_t n_chunks, bool is_block_chunk) const {
        if (is_block_chunk && !block_tracker_initd) {
            block_tracker = BlockChunkRecvdTracker(n_chunks);
            block_tracker_initd = true;
        }

        if (!is_block_chunk && !header_tracker_initd) {
            header_tracker = BlockChunkRecvdTracker(n_chunks);
            header_tracker_initd = true;
        }
    }

public:
    ChunksAvailableSet(bool hasAllChunks, size_t n_chunks, bool is_block_chunk) :
        allSent(hasAllChunks), header_tracker_initd(!is_block_chunk),
        block_tracker_initd(is_block_chunk) {
            if (allSent) return;
            InitTracker(n_chunks, is_block_chunk);
    }

    bool IsChunkAvailable(uint32_t chunk_id, size_t n_chunks, bool is_block_chunk) const {
        if (allSent) return true;

        InitTracker(n_chunks, is_block_chunk);

        if (is_block_chunk) {
            assert(block_tracker_initd);
            return block_tracker.CheckPresent(chunk_id);
        } else {
            assert(header_tracker_initd);
            return header_tracker.CheckPresent(chunk_id);
        }
    }

    void SetChunkAvailable(uint32_t chunk_id, size_t n_chunks, bool is_block_chunk) {
        if (allSent) return;

        InitTracker(n_chunks, is_block_chunk);

        if (is_block_chunk) {
            assert(block_tracker_initd);
            block_tracker.CheckPresentAndMarkRecvd(chunk_id);
        } else {
            assert(header_tracker_initd);
            header_tracker.CheckPresentAndMarkRecvd(chunk_id);
        }
    }

    void SetAllAvailable() { allSent = true; }
    bool AreAllAvailable() const { return allSent; }
};

struct UDPConnectionInfo {
    uint64_t local_magic;  // Already LE
    uint64_t remote_magic; // Already LE
    size_t group;
    bool fTrusted;
    UDPConnectionType connection_type;
    udp_mode_t udp_mode;
};

struct UDPMulticastStats {
    std::chrono::steady_clock::time_point lastRxTime = std::chrono::steady_clock::now();
    int64_t rcvdBytes = 0;
};

struct UDPMulticastInfo {
    char ifname[IFNAMSIZ];          /** network interface name */
    char mcast_ip[INET_ADDRSTRLEN]; /** multicast IPv4 address */
    unsigned short port;            /** UDP port */
    size_t group;                   /** UDP group */
    bool tx;                        /** multicast Tx or Rx? */
    int fd;                         /** socket file descriptor */
    mutable UDPMulticastStats stats;/** statistics */
    /* Rx only: */
    char tx_ip[INET_ADDRSTRLEN];    /** source IPv4 address (sender address) */
    std::string groupname;          /** optional label for stream */
    bool trusted;                   /** whether multicast Tx peer is trusted */
    /* Tx only: */
    int ttl;               /** time-to-live desired for multicast packets */
    uint64_t bw;           /** target throughput in bps */
    int depth;             /** backfill depth - no. of blocks to iterate over */
    int interleave_size;   /** determines the depth of the sub-window of blocks
                            *  within the backfill window whose FEC chunks are
                            *  intereleaved. This depth is given in terms of the
                            *  total size of all blocks in the sub-window. */
    uint16_t physical_idx; /** index of destination IP - net interface pair */
    uint16_t logical_idx;  /** logical idx for streams sharing physical idx */
    int txn_per_sec;       /** txns to send per second (0 to disable) */
    char dscp;             /** Differentiated Services Code Point (DSCP) */
};

struct UDPConnectionState {
    UDPConnectionInfo connection;
    int state; // Flags from UDPState
    uint32_t protocolVersion;
    int64_t lastSendTime;
    int64_t lastRecvTime;
    int64_t lastPingTime;
    std::map<uint64_t, int64_t> ping_times;
    double last_pings[10];
    unsigned int last_ping_location;
    std::map<uint64_t, ChunksAvailableSet> chunks_avail;
    uint64_t tx_in_flight_hash_prefix, tx_in_flight_msg_size;
    std::unique_ptr<FECDecoder> tx_in_flight;

    UDPConnectionState() : connection({}), state(0), protocolVersion(0), lastSendTime(0), lastRecvTime(0), lastPingTime(0), last_ping_location(0),
        tx_in_flight_hash_prefix(0), tx_in_flight_msg_size(0)
        { for (size_t i = 0; i < sizeof(last_pings) / sizeof(double); i++) last_pings[i] = -1; }
};
#define PROTOCOL_VERSION_MIN(ver) (((ver) >> 16) & 0xffff)
#define PROTOCOL_VERSION_CUR(ver) (((ver) >>  0) & 0xffff)
#define PROTOCOL_VERSION_FLAGS(ver) (((ver) >> 32) & 0xffffffff)

extern std::recursive_mutex cs_mapUDPNodes;
extern std::map<CService, UDPConnectionState> mapUDPNodes;
extern std::atomic<uint64_t> min_per_node_mbps; // Used to determine header FEC chunk count
extern bool maybe_have_write_nodes;
extern uint64_t const multicast_checksum_magic;

void SendMessage(const UDPMessage& msg, const unsigned int length, bool high_prio, const CService& service, const uint64_t magic, size_t group);
void SendMessage(const UDPMessage& msg, const unsigned int length, bool high_prio, const std::pair<const CService, UDPConnectionState>& node);
void DisconnectNode(const std::map<CService, UDPConnectionState>::iterator& it);

const std::map<std::tuple<CService, int, uint16_t>, UDPMulticastInfo>& multicast_nodes();
bool IsMulticastRxNode(const CService&);

#endif
