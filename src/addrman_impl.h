// Copyright (c) 2012-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ADDRMAN_IMPL_H
#define BITCOIN_ADDRMAN_IMPL_H

namespace {

//! Serialization versions.
enum class Format : uint8_t {
    V0_HISTORICAL = 0,    //!< historic format, before commit e6b343d88
    V1_DETERMINISTIC = 1, //!< for pre-asmap files
    V2_ASMAP = 2,         //!< for files including asmap version
    V3_BIP155 = 3,        //!< same as V2_ASMAP plus addresses are in BIP155 format
};

} // unnamed namespace

/** Extended statistics about a CAddress */
class CAddrInfo : public CAddress
{
public:
    //! last try whatsoever by us (memory only)
    int64_t nLastTry{0};

    //! last counted attempt (memory only)
    int64_t nLastCountAttempt{0};

    //! where knowledge about this address first came from
    CNetAddr source;

    //! last successful connection by us
    int64_t nLastSuccess{0};

    //! connection attempts since last successful attempt
    int nAttempts{0};

    //! reference count in new sets (memory only)
    int nRefCount{0};

    //! in tried set? (memory only)
    bool fInTried{false};

    //! position in vRandom
    int nRandomPos{-1};

    friend class CAddrMan;

    SERIALIZE_METHODS(CAddrInfo, obj)
    {
        READWRITEAS(CAddress, obj);
        READWRITE(obj.source, obj.nLastSuccess, obj.nAttempts);
    }

    CAddrInfo(const CAddress &addrIn, const CNetAddr &addrSource) : CAddress(addrIn), source(addrSource)
    {
    }

    CAddrInfo() : CAddress(), source()
    {
    }

    //! Calculate in which "tried" bucket this entry belongs
    int GetTriedBucket(const uint256 &nKey, const std::vector<bool> &asmap) const
    {
        uint64_t hash1 = (CHashWriter(SER_GETHASH, 0) << nKey << GetKey()).GetCheapHash();
        uint64_t hash2 = (CHashWriter(SER_GETHASH, 0) << nKey << GetGroup(asmap) << (hash1 % ADDRMAN_TRIED_BUCKETS_PER_GROUP)).GetCheapHash();
        int tried_bucket = hash2 % ADDRMAN_TRIED_BUCKET_COUNT;
        uint32_t mapped_as = GetMappedAS(asmap);
        LogPrint(BCLog::NET, "IP %s mapped to AS%i belongs to tried bucket %i\n", ToStringIP(), mapped_as, tried_bucket);
        return tried_bucket;
    }

    //! Calculate in which "new" bucket this entry belongs, given a certain source
    int GetNewBucket(const uint256 &nKey, const CNetAddr& src, const std::vector<bool> &asmap) const
    {
        std::vector<unsigned char> vchSourceGroupKey = src.GetGroup(asmap);
        uint64_t hash1 = (CHashWriter(SER_GETHASH, 0) << nKey << GetGroup(asmap) << vchSourceGroupKey).GetCheapHash();
        uint64_t hash2 = (CHashWriter(SER_GETHASH, 0) << nKey << vchSourceGroupKey << (hash1 % ADDRMAN_NEW_BUCKETS_PER_SOURCE_GROUP)).GetCheapHash();
        int new_bucket = hash2 % ADDRMAN_NEW_BUCKET_COUNT;
        uint32_t mapped_as = GetMappedAS(asmap);
        LogPrint(BCLog::NET, "IP %s mapped to AS%i belongs to new bucket %i\n", ToStringIP(), mapped_as, new_bucket);
        return new_bucket;
    }

    //! Calculate in which "new" bucket this entry belongs, using its default source
    int GetNewBucket(const uint256 &nKey, const std::vector<bool> &asmap) const
    {
        return GetNewBucket(nKey, source, asmap);
    }

    //! Calculate in which position of a bucket to store this entry.
    int GetBucketPosition(const uint256 &nKey, bool fNew, int nBucket) const
    {
        uint64_t hash1 = (CHashWriter(SER_GETHASH, 0) << nKey << (fNew ? 'N' : 'K') << nBucket << GetKey()).GetCheapHash();
        return hash1 % ADDRMAN_BUCKET_SIZE;
    }

    //! Determine whether the statistics about this entry are bad enough so that it can just be deleted
    bool IsTerrible(int64_t nNow = GetAdjustedTime()) const
    {
        if (nLastTry && nLastTry >= nNow - 60) // never remove things tried in the last minute
            return false;

        if (nTime > nNow + 10 * 60) // came in a flying DeLorean
            return true;

        if (nTime == 0 || nNow - nTime > ADDRMAN_HORIZON_DAYS * 24 * 60 * 60) // not seen in recent history
            return true;

        if (nLastSuccess == 0 && nAttempts >= ADDRMAN_RETRIES) // tried N times and never a success
            return true;

        if (nNow - nLastSuccess > ADDRMAN_MIN_FAIL_DAYS * 24 * 60 * 60 && nAttempts >= ADDRMAN_MAX_FAILURES) // N successive failures in the last week
            return true;

        return false;
    }

    //! Calculate the relative chance this entry should be given when selecting nodes to connect to
    double GetChance(int64_t nNow = GetAdjustedTime()) const
    {
        double fChance = 1.0;
        int64_t nSinceLastTry = std::max<int64_t>(nNow - nLastTry, 0);

        // deprioritize very recent attempts away
        if (nSinceLastTry < 60 * 10)
            fChance *= 0.01;

        // deprioritize 66% after each failed attempt, but at most 1/28th to avoid the search taking forever or overly penalizing outages.
        fChance *= pow(0.66, std::min(nAttempts, 8));

        return fChance;
    }
};

class AddrManImpl
{
public:
    //! critical section to protect the inner data structures
    mutable RecursiveMutex cs;

    //! last used nId
    int nIdCount GUARDED_BY(cs);

    //! table with information about all nIds
    std::map<int, CAddrInfo> mapInfo GUARDED_BY(cs);

    //! find an nId based on its network address
    std::map<CNetAddr, int> mapAddr GUARDED_BY(cs);

    //! randomly-ordered vector of all nIds
    std::vector<int> vRandom GUARDED_BY(cs);

    // number of "tried" entries
    int nTried GUARDED_BY(cs);

    //! list of "tried" buckets
    int vvTried[ADDRMAN_TRIED_BUCKET_COUNT][ADDRMAN_BUCKET_SIZE] GUARDED_BY(cs);

    //! number of (unique) "new" entries
    int nNew GUARDED_BY(cs);

    //! list of "new" buckets
    int vvNew[ADDRMAN_NEW_BUCKET_COUNT][ADDRMAN_BUCKET_SIZE] GUARDED_BY(cs);

    //! last time Good was called (memory only)
    int64_t nLastGood GUARDED_BY(cs);

    //! Holds addrs inserted into tried table that collide with existing entries. Test-before-evict discipline used to resolve these collisions.
    std::set<int> m_tried_collisions;

    /** Whether to perform sanity checks before and after each operation. */
    const bool m_consistency_check{false};

    //! secret key to randomize bucket select with
    uint256 nKey;

    //! Source of random numbers for randomization in inner loops
    FastRandomContext insecure_rand;

    //! Find an entry.
    CAddrInfo* Find(const CNetAddr& addr, int *pnId = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! find an entry, creating it if necessary.
    //! nTime and nServices of the found node are updated, if necessary.
    CAddrInfo* Create(const CAddress &addr, const CNetAddr &addrSource, int *pnId = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Swap two elements in vRandom.
    void SwapRandom(unsigned int nRandomPos1, unsigned int nRandomPos2) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Move an entry from the "new" table(s) to the "tried" table
    void MakeTried(CAddrInfo& info, int nId) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Delete an entry. It must not be in tried, and have refcount 0.
    void Delete(int nId) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Clear a position in a "new" table. This is the only place where entries are actually deleted.
    void ClearNew(int nUBucket, int nUBucketPos) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Mark an entry "good", possibly moving it from "new" to "tried".
    void Good_(const CService &addr, bool test_before_evict, int64_t time) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Add an entry to the "new" table.
    bool Add_(const CAddress &addr, const CNetAddr& source, int64_t nTimePenalty) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Mark an entry as attempted to connect.
    void Attempt_(const CService &addr, bool fCountFailure, int64_t nTime) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Select an address to connect to, if newOnly is set to true, only the new table is selected from.
    CAddrInfo Select_(bool newOnly) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! See if any to-be-evicted tried table entries have been tested and if so resolve the collisions.
    void ResolveCollisions_() EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Return a random to-be-evicted tried table address.
    CAddrInfo SelectTriedCollision_() EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Perform internal consistency check. Asserts if any invariant fails.
    void ConsistencyCheck() EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Select several addresses at once.
    void GetAddr_(std::vector<CAddress> &vAddr, size_t max_addresses, size_t max_pct) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Mark an entry as currently-connected-to.
    void Connected_(const CService &addr, int64_t nTime) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Update an entry's service bits.
    void SetServices_(const CService &addr, ServiceFlags nServices) EXCLUSIVE_LOCKS_REQUIRED(cs);

public:
    // Read asmap from provided binary file
    static std::vector<bool> DecodeAsmap(fs::path path);

    template <typename Stream>
    void Serialize(Stream& s_) const;

    template <typename Stream>
    void Unserialize(Stream& s_);

    AddrManImpl(bool deterministic=false, bool consistency_check=false) : m_consistency_check(consistency_check)
    {
        Clear(deterministic);
    }

    ~AddrManImpl()
    {
        nKey.SetNull();
    }

    //! Clear all data from this addrman.
    void Clear(bool deterministic=false);

    //! Return the number of (unique) addresses in all tables.
    size_t size() const
    {
        LOCK(cs); // TODO: Cache this in an atomic to avoid this overhead
        return vRandom.size();
    }

    //! Add a single address.
    bool Add(const CAddress &addr, const CNetAddr& source, int64_t nTimePenalty = 0)
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        bool fRet = Add_(addr, source, nTimePenalty);
        if (fRet) {
            LogPrint(BCLog::ADDRMAN, "Added %s from %s: %i tried, %i new\n", addr.ToStringIPPort(), source.ToString(), nTried, nNew);
        }
        if (m_consistency_check) ConsistencyCheck();
        return fRet;
    }

    //! Add multiple addresses.
    bool Add(const std::vector<CAddress> &vAddr, const CNetAddr& source, int64_t nTimePenalty = 0)
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        int nAdd = 0;
        for (std::vector<CAddress>::const_iterator it = vAddr.begin(); it != vAddr.end(); it++) {
            nAdd += Add_(*it, source, nTimePenalty) ? 1 : 0;
        }
        if (nAdd) {
            LogPrint(BCLog::ADDRMAN, "Added %i addresses from %s: %i tried, %i new\n", nAdd, source.ToString(), nTried, nNew);
        }
        if (m_consistency_check) ConsistencyCheck();
        return nAdd > 0;
    }

    //! Mark an entry as accessible.
    void Good(const CService &addr, bool test_before_evict = true, int64_t nTime = GetAdjustedTime())
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        Good_(addr, test_before_evict, nTime);
        if (m_consistency_check) ConsistencyCheck();
    }

    //! Mark an entry as connection attempted to.
    void Attempt(const CService &addr, bool fCountFailure, int64_t nTime = GetAdjustedTime())
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        Attempt_(addr, fCountFailure, nTime);
        if (m_consistency_check) ConsistencyCheck();
    }

    //! See if any to-be-evicted tried table entries have been tested and if so resolve the collisions.
    void ResolveCollisions()
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        ResolveCollisions_();
        if (m_consistency_check) ConsistencyCheck();
    }

    //! Randomly select an address in tried that another address is attempting to evict.
    CAddrInfo SelectTriedCollision()
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        CAddrInfo ret = SelectTriedCollision_();
        if (m_consistency_check) ConsistencyCheck();
        return ret;
    }

    /**
     * Choose an address to connect to.
     */
    CAddrInfo Select(bool newOnly = false)
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        CAddrInfo addrRet = Select_(newOnly);
        if (m_consistency_check) ConsistencyCheck();
        return addrRet;
    }

    //! Return a bunch of addresses, selected at random.
    std::vector<CAddress> GetAddr(size_t max_addresses, size_t max_pct)
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        std::vector<CAddress> vAddr;
        GetAddr_(vAddr, max_addresses, max_pct);
        if (m_consistency_check) ConsistencyCheck();
        return vAddr;
    }

    //! Mark an entry as currently-connected-to.
    void Connected(const CService &addr, int64_t nTime = GetAdjustedTime())
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        Connected_(addr, nTime);
        if (m_consistency_check) ConsistencyCheck();
    }

    void SetServices(const CService &addr, ServiceFlags nServices)
    {
        LOCK(cs);
        if (m_consistency_check) ConsistencyCheck();
        SetServices_(addr, nServices);
        if (m_consistency_check) ConsistencyCheck();
    }

};

#endif // BITCOIN_ADDRMAN_H
#endif // BITCOIN_ADDRMAN_IMPL_H
