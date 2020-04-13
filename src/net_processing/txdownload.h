// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDOWNLOAD_H
#define BITCOIN_TXDOWNLOAD_H

#include <sync.h>
#include <uint256.h>
#include <util/transaction.h>

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

extern RecursiveMutex cs_main;

/** How long to wait (in microseconds) before downloading a transaction from an additional peer */
static constexpr std::chrono::microseconds GETDATA_TX_INTERVAL{std::chrono::seconds{60}};
/** Maximum number of in-flight transactions from a peer */
static constexpr int32_t MAX_PEER_TX_IN_FLIGHT = 100;

/** A transaction that has been announced to us by a single peer. We store
 *  the txid and the request time.  */
struct AnnouncedTx {
    /** The txid of the announced transaction. */
    uint256 m_hash;

    /** The timestamp for requesting the transaction from this peer:
     *  - for transactions which are announced but not yet requested,
     *    this is the next time that we'll consider downloading the
     *    transaction from this peer.
     *  - for AnnouncedTx which we've requested, this is the time that we
     *    requested the transaction from this peer. */
    std::chrono::microseconds m_timestamp;

    AnnouncedTx(uint256 hash, std::chrono::microseconds timestamp) :
        m_hash(hash), m_timestamp(timestamp)  {}
};

/** Compare function for AnnouncedTxs. Sorts first on the request time, and then
    on txid as a tiebreaker. */
struct AnnouncedTxTimeCompare
{
    bool operator()(const std::shared_ptr<AnnouncedTx> lhs, const std::shared_ptr<AnnouncedTx> rhs) const
    {
        return lhs->m_timestamp < rhs->m_timestamp ||
            (lhs->m_timestamp == rhs->m_timestamp && lhs->m_hash < rhs->m_hash);
    }
};

/*
* State associated with transaction download.
 *
 * Tx download algorithm:
 *
 *   When inv comes in, queue up (process_time, txid) inside the peer's
 *   CNodeState (m_tx_process_time) as long as m_tx_announced for the peer
 *   isn't too big (MAX_PEER_TX_ANNOUNCEMENTS).
 *
 *   The process_time for a transaction is set to nNow for outbound peers,
 *   nNow + 2 seconds for inbound peers. This is the time at which we'll
 *   consider trying to request the transaction from the peer in
 *   SendMessages(). The delay for inbound peers is to allow outbound peers
 *   a chance to announce before we request from inbound peers, to prevent
 *   an adversary from using inbound connections to blind us to a
 *   transaction (InvBlock).
 *
 *   When we call SendMessages() for a given peer,
 *   we will loop over the transactions in m_tx_process_time, looking
 *   at the transactions whose process_time <= nNow. We'll request each
 *   such transaction that we don't have already and that hasn't been
 *   requested from another peer recently, up until we hit the
 *   MAX_PEER_TX_IN_FLIGHT limit for the peer. Then we'll update
 *   g_already_asked_for for each requested txid, storing the time of the
 *   GETDATA request. We use g_already_asked_for to coordinate transaction
 *   requests amongst our peers.
 *
 *   For transactions that we still need but we have already recently
 *   requested from some other peer, we'll reinsert (process_time, txid)
 *   back into the peer's m_tx_process_time at the point in the future at
 *   which the most recent GETDATA request would time out (ie
 *   GETDATA_TX_INTERVAL + the request time stored in g_already_asked_for).
 *   We add an additional delay for inbound peers, again to prefer
 *   attempting download from outbound peers first.
 *   We also add an extra small random delay up to 2 seconds
 *   to avoid biasing some peers over others. (e.g., due to fixed ordering
 *   of peer processing in ThreadMessageHandler).
 *
 *   When we receive a transaction from a peer, we remove the txid from the
 *   peer's m_tx_in_flight set and from their recently announced set
 *   (m_tx_announced).  We also clear g_already_asked_for for that entry, so
 *   that if somehow the transaction is not accepted but also not added to
 *   the reject filter, then we will eventually redownload from other
 *   peers.
 */
class TxDownloadState {
private:
    /** All transactions that have been announced by this peer, ordered by hash */
    std::unordered_map<uint256, std::shared_ptr<AnnouncedTx>, SaltedTxidHasher> m_txs;

    /** Transactions that have been announced that we haven't requested from this
     *  peer, ordered by request time */
    std::set<std::shared_ptr<AnnouncedTx>, AnnouncedTxTimeCompare> m_announced_txs;

    /** Transactions that we have requested from this peer, ordered by
     *  request time */
    std::set<std::shared_ptr<AnnouncedTx>, AnnouncedTxTimeCompare> m_requested_txs;

    /** Periodically check for stuck getdata requests */
    std::chrono::microseconds m_check_expiry_timer{0};

public:
    /** The peer has sent us an INV. Keep track of the hash and when to request
     *  the transaction from this peer. */
    void AddAnnouncedTx(uint256 hash, std::chrono::microseconds request_time);

    /** We have requested this transaction from another peer. Reset this
     *  peer's request time for this transaction to after the outstanding
     *  request times out. */
    void RequeueTx(uint256 hash, std::chrono::microseconds request_time);

    /** We sent this peer a GETDATA for this transaction. Save the request
     *  time so we can expire it if the peer doesn't respond. */
    void RequestSent(uint256 hash, std::chrono::microseconds request_time);

    /** Transaction has either been received or expired. No longer request
     * it from this peer. */
    void RemoveTx(uint256 hash);

    /** Expire old requests after a long timeout, so that we can resume
     * downloading transactions from a peer even if they were unresponsive in
     * the past. */
    void ExpireOldAnnouncedTxs(std::chrono::microseconds current_time, std::vector<uint256>& expired_requests);

    /** Get the next transaction to request and remove it from the list of txids to be requested.
     *  Returns false if there are currently no more transactions to request.   */
    bool GetAnnouncedTxToRequest(std::chrono::microseconds current_time, uint256& txid);
};

void EraseTxRequest(const uint256& txid) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

std::chrono::microseconds GetTxRequestTime(const uint256& txid) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

void UpdateTxRequestTime(const uint256& txid, std::chrono::microseconds request_time) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

std::chrono::microseconds CalculateTxGetDataTime(const uint256& txid, std::chrono::microseconds current_time, bool use_inbound_delay) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

#endif // BITCOIN_TXDOWNLOAD_H
