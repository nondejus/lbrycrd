#include <claimtrie.h>
#include <coins.h>
#include <core_io.h>
#include <key_io.h>
#include <logging.h>
#include <nameclaim.h>
#include <rpc/claimrpchelp.h>
#include <rpc/server.h>
#include <script/standard.h>
#include <shutdown.h>
#include <txdb.h>
#include <txmempool.h>
#include <undo.h>
#include <univalue.h>
#include <validation.h>

#include <boost/locale.hpp>
#include <boost/locale/conversion.hpp>
#include <boost/thread.hpp>
#include <cmath>

static constexpr size_t claimIdHexLength = 40;

uint160 uint160S(const std::string& str)
{
    uint160 s;
    s.SetHex(str);
    return s;
}

void ParseClaimtrieId(const UniValue& v, std::string& claimId, const std::string& strName)
{
    // use IsHexNumber which verify odd strings size
    if (!v.isStr() || !IsHexNumber(claimId = v.get_str()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be a hexadecimal string");
    if (claimId.length() > claimIdHexLength)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be max 20-character hexadecimal string");
}

static CBlockIndex* BlockHashIndex(const uint256& blockHash)
{
    AssertLockHeld(cs_main);

    if (mapBlockIndex.count(blockHash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlockIndex* pblockIndex = mapBlockIndex[blockHash];
    if (!chainActive.Contains(pblockIndex))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Block not in main chain");

    return pblockIndex;
}

#define MAX_RPC_BLOCK_DECREMENTS 500

extern CChainState g_chainstate;
void RollBackTo(const CBlockIndex* targetIndex, CCoinsViewCache& coinsCache, CClaimTrieCache& trieCache)
{
    AssertLockHeld(cs_main);

    const CBlockIndex* activeIndex = chainActive.Tip();

    if (activeIndex->nHeight > (targetIndex->nHeight + MAX_RPC_BLOCK_DECREMENTS))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Block is too deep");

    const size_t currentMemoryUsage = pcoinsTip->DynamicMemoryUsage();

    for (; activeIndex && activeIndex != targetIndex; activeIndex = activeIndex->pprev) {
        boost::this_thread::interruption_point();

        CBlock block;
        if (!ReadBlockFromDisk(block, activeIndex, Params().GetConsensus()))
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Failed to read %s", activeIndex->ToString()));

        if (coinsCache.DynamicMemoryUsage() + currentMemoryUsage > nCoinCacheUsage)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Out of memory, you may want to increase dbcache size");

        if (ShutdownRequested())
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Shutdown requested");

        if (g_chainstate.DisconnectBlock(block, activeIndex, coinsCache, trieCache) != DisconnectResult::DISCONNECT_OK)
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Failed to disconnect %s", block.ToString()));
    }
    trieCache.getMerkleHash(); // update the hash tree
}

std::string escapeNonUtf8(const std::string& name)
{
    using namespace boost::locale::conv;
    try {
        return to_utf<char>(name, "UTF-8", stop);
    } catch (const conversion_error&) {
        std::string result;
        result.reserve(name.size() * 2);
        for (uint8_t ch : name) {
            if (ch < 0x08 || (ch >= 0x0e && ch <= 0x1f) || ch >= 0x7f)
                result += tfm::format("\\u%04x", ch);
            else if (ch == 0x08) result += "\\b";
            else if (ch == 0x09) result += "\\t";
            else if (ch == 0x0a) result += "\\n";
            else if (ch == 0x0c) result += "\\f";
            else if (ch == 0x0d) result += "\\r";
            else if (ch == 0x22) result += "\\\"";
            else if (ch == 0x5c) result += "\\\\";
            else result += ch;
        }
        return result;
    }
}

static bool extractValue(const CScript& scriptPubKey, std::string& sValue)
{
    int op;
    std::vector<std::vector<unsigned char>> vvchParams;
    if (!DecodeClaimScript(scriptPubKey, op, vvchParams))
        return false;

    if (op == OP_CLAIM_NAME)
        sValue = HexStr(vvchParams[1].begin(), vvchParams[1].end());
    else if (vvchParams.size() > 2) // both UPDATE and SUPPORT
        sValue = HexStr(vvchParams[2].begin(), vvchParams[2].end());
    else
        return false;
    return true;
}

bool getClaimById(const uint160& claimId, std::string& name, CClaimValue* claim = nullptr)
{
    if (claimId.IsNull())
        return false;

    CClaimIndexElement element;
    if (!pclaimTrie->db->Read(std::make_pair(CLAIM_BY_ID, claimId), element))
        return false;
    if (element.claim.claimId == claimId) {
        name = element.name;
        if (claim)
            *claim = element.claim;
        return true;
    }
    return false;
}

// name can be setted explicitly
bool getClaimById(const std::string& partialId, std::string& name, CClaimValue* claim = nullptr)
{
    if (partialId.empty())
        return false;

    std::unique_ptr<CDBIterator> pcursor(pclaimTrie->db->NewIterator());

    for (pcursor->SeekToFirst(); pcursor->Valid(); pcursor->Next()) {
        std::pair<uint8_t, uint160> key;
        if (!pcursor->GetKey(key) || key.first != CLAIM_BY_ID)
            continue;

        if (key.second.GetHex().find(partialId) != 0)
            continue;

        CClaimIndexElement element;
        if (pcursor->GetValue(element)) {
            if (!name.empty() && name != element.name)
                continue;
            name = element.name;
            if (claim)
                *claim = element.claim;
            return true;
        }
    }
    return false;
}

std::vector<CClaimNsupports> seqSort(const std::vector<CClaimNsupports>& source)
{
    auto claimsNsupports = source;

    std::sort(claimsNsupports.begin(), claimsNsupports.end(), [](const CClaimNsupports& lhs, const CClaimNsupports& rhs) {
        auto& lc = lhs.claim;
        auto& rc = rhs.claim;
        return lc.nHeight < rc.nHeight || (lc.nHeight == rc.nHeight && lc.outPoint.n < rc.outPoint.n);
    });

    return claimsNsupports;
}

std::size_t indexOf(const std::vector<CClaimNsupports>& source, const uint160& claimId)
{
    auto it = std::find_if(source.begin(), source.end(), [&claimId](const CClaimNsupports& claimNsupports) {
        return claimNsupports.claim.claimId == claimId;
    });
    assert(it != source.end());
    return std::distance(source.begin(), it);
}

UniValue claimToJSON(const CCoinsViewCache& coinsCache, const CClaimValue& claim)
{
    UniValue result(UniValue::VOBJ);

    std::string targetName;
    if (getClaimById(claim.claimId, targetName))
        result.pushKV(T_NAME, escapeNonUtf8(targetName));

    auto& coin = coinsCache.AccessCoin(claim.outPoint);
    if (!coin.IsSpent()) {
        std::string value;
        if (extractValue(coin.out.scriptPubKey, value))
            result.pushKV(T_VALUE, value);

        CTxDestination address;
        if (ExtractDestination(coin.out.scriptPubKey, address))
            result.pushKV(T_ADDRESS, EncodeDestination(address));
    }

    result.pushKV(T_CLAIMID, claim.claimId.GetHex());
    result.pushKV(T_TXID, claim.outPoint.hash.GetHex());
    result.pushKV(T_N, (int)claim.outPoint.n);
    result.pushKV(T_HEIGHT, claim.nHeight);
    result.pushKV(T_VALIDATHEIGHT, claim.nValidAtHeight);
    result.pushKV(T_AMOUNT, claim.nAmount);

    return result;
}

UniValue supportToJSON(const CCoinsViewCache& coinsCache, const CSupportValue& support)
{
    UniValue ret(UniValue::VOBJ);

    auto& coin = coinsCache.AccessCoin(support.outPoint);
    if (!coin.IsSpent()) {
        std::string value;
        if (extractValue(coin.out.scriptPubKey, value))
            ret.pushKV(T_VALUE, value);

        CTxDestination address;
        if (ExtractDestination(coin.out.scriptPubKey, address))
            ret.pushKV(T_ADDRESS, EncodeDestination(address));
    }

    ret.pushKV(T_TXID, support.outPoint.hash.GetHex());
    ret.pushKV(T_N, (int)support.outPoint.n);
    ret.pushKV(T_HEIGHT, support.nHeight);
    ret.pushKV(T_VALIDATHEIGHT, support.nValidAtHeight);
    ret.pushKV(T_AMOUNT, support.nAmount);

    return ret;
}

CAmount amountToClaim(const CClaimNsupports& claimNsupports)
{
    auto fullAmount = claimNsupports.claim.nAmount;
    for (auto& support : claimNsupports.supports)
        fullAmount += support.nAmount;
    return fullAmount;
}

UniValue claimAndSupportsToJSON(const CCoinsViewCache& coinsCache, const CClaimNsupports& claimNsupports)
{
    auto& claim = claimNsupports.claim;
    auto& supports = claimNsupports.supports;

    auto result = claimToJSON(coinsCache, claim);
    result.pushKV(T_EFFECTIVEAMOUNT, claimNsupports.effectiveAmount);

    auto fullAmount = amountToClaim(claimNsupports);
    if (fullAmount > claimNsupports.effectiveAmount)
        result.pushKV(T_PENDINGAMOUNT, fullAmount);

    UniValue supportObjs(UniValue::VARR);
    for (auto& support : supports)
        supportObjs.push_back(supportToJSON(coinsCache, support));

    result.pushKV(T_SUPPORTS, supportObjs);

    return result;
}

bool validParams(const UniValue& params, uint8_t required, uint8_t optional)
{
    auto count = params.size();
    return count >= required && count <= required + optional;
}

void validateRequest(const JSONRPCRequest& request, int findex, uint8_t required, uint8_t optional)
{
    if (request.fHelp || !validParams(request.params, required, optional))
        throw std::runtime_error(rpc_help[findex]);
}

static UniValue getclaimsintrie(const JSONRPCRequest& request)
{
    validateRequest(request, GETCLAIMSINTRIE, 0, 1);

    if (!IsDeprecatedRPCEnabled("getclaimsintrie")) {
        const auto msg = "getclaimsintrie is deprecated and will be removed in v0.18. To use this command, start with -deprecatedrpc=getclaimsintrie";
        if (request.fHelp)
            throw std::runtime_error(msg);
        throw JSONRPCError(RPC_METHOD_DEPRECATED, msg);
    }

    LOCK(cs_main);
    CCoinsViewCache coinsCache(pcoinsTip.get());
    CClaimTrieCache trieCache(pclaimTrie);

    if (!request.params.empty()) {
        CBlockIndex* blockIndex = BlockHashIndex(ParseHashV(request.params[0], T_BLOCKHASH " (optional parameter 1)"));
        RollBackTo(blockIndex, coinsCache, trieCache);
    }

    UniValue ret(UniValue::VARR);
    trieCache.iterate([&ret, &trieCache, &coinsCache] (const std::string& name, const CClaimTrieData& data) {
        if (ShutdownRequested())
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Shutdown requested");

        boost::this_thread::interruption_point();

        if (data.empty())
            return;

        UniValue claims(UniValue::VARR);
        for (auto& claim : data.claims)
            claims.push_back(claimToJSON(coinsCache, claim));

        UniValue nodeObj(UniValue::VOBJ);
        nodeObj.pushKV(T_NORMALIZEDNAME, escapeNonUtf8(name));
        nodeObj.pushKV(T_CLAIMS, claims);
        ret.push_back(nodeObj);
    });
    return ret;
}

static UniValue getclaimtrie(const JSONRPCRequest& request)
{
    throw JSONRPCError(RPC_METHOD_DEPRECATED, "getclaimtrie was removed in v0.17.\n"
        "Clients should use getnamesintrie.");
}

static UniValue getnamesintrie(const JSONRPCRequest& request)
{
    validateRequest(request, GETNAMESINTRIE, 0, 1);

    LOCK(cs_main);
    CCoinsViewCache coinsCache(pcoinsTip.get());
    CClaimTrieCache trieCache(pclaimTrie);

    if (!request.params.empty()) {
        CBlockIndex* blockIndex = BlockHashIndex(ParseHashV(request.params[0], T_BLOCKHASH " (optional parameter 1)"));
        RollBackTo(blockIndex, coinsCache, trieCache);
    }

    UniValue ret(UniValue::VARR);
    trieCache.iterate([&ret](const std::string &name, const CClaimTrieData &data) {
        if (!data.empty())
            ret.push_back(escapeNonUtf8(name));
        if (ShutdownRequested())
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Shutdown requested");

        boost::this_thread::interruption_point();
    });

    return ret;
}

static UniValue getvalueforname(const JSONRPCRequest& request)
{
    validateRequest(request, GETVALUEFORNAME, 1, 2);

    LOCK(cs_main);
    CCoinsViewCache coinsCache(pcoinsTip.get());
    CClaimTrieCache trieCache(pclaimTrie);

    if (request.params.size() > 1) {
        CBlockIndex* blockIndex = BlockHashIndex(ParseHashV(request.params[1], T_BLOCKHASH " (optional parameter 2)"));
        RollBackTo(blockIndex, coinsCache, trieCache);
    }

    std::string claimId;
    if (request.params.size() > 2)
        ParseClaimtrieId(request.params[2], claimId, T_CLAIMID " (optional parameter 3)");

    const auto name = request.params[0].get_str();
    UniValue ret(UniValue::VOBJ);

    auto csToName = trieCache.getClaimsForName(name);
    if (csToName.claimsNsupports.empty())
        return ret;

    auto& claimNsupports =
        claimId.length() == claimIdHexLength ? csToName.find(uint160S(claimId)) :
        !claimId.empty() ? csToName.find(claimId) : csToName.claimsNsupports[0];

    if (claimNsupports.IsNull())
        return ret;

    std::size_t seq = 0, bid = 0;
    if (csToName.claimsNsupports.size() > 1) {
        auto& claimId = claimNsupports.claim.claimId;
        auto seqOrder = seqSort(csToName.claimsNsupports);
        seq = indexOf(seqOrder, claimId);
        bid = indexOf(csToName.claimsNsupports, claimId);
    }

    ret.pushKV(T_NORMALIZEDNAME, escapeNonUtf8(csToName.name));
    ret.pushKVs(claimAndSupportsToJSON(coinsCache, claimNsupports));
    ret.pushKV(T_LASTTAKEOVERHEIGHT, csToName.nLastTakeoverHeight);
    ret.pushKV(T_BID, (int)bid);
    ret.pushKV(T_SEQUENCE, (int)seq);

    return ret;
}

UniValue getclaimsforname(const JSONRPCRequest& request)
{
    validateRequest(request, GETCLAIMSFORNAME, 1, 1);

    LOCK(cs_main);
    CCoinsViewCache coinsCache(pcoinsTip.get());
    CClaimTrieCache trieCache(pclaimTrie);

    if (request.params.size() > 1) {
        CBlockIndex* blockIndex = BlockHashIndex(ParseHashV(request.params[1], T_BLOCKHASH " (optional parameter 2)"));
        RollBackTo(blockIndex, coinsCache, trieCache);
    }

    std::string name = request.params[0].get_str();
    auto csToName = trieCache.getClaimsForName(name);

    UniValue result(UniValue::VOBJ);
    result.pushKV(T_NORMALIZEDNAME, escapeNonUtf8(csToName.name));

    auto seqOrder = seqSort(csToName.claimsNsupports);

    UniValue claimObjs(UniValue::VARR);
    for (std::size_t i = 0; i < csToName.claimsNsupports.size(); ++i) {
        auto& claimNsupports = csToName.claimsNsupports[i];
        auto claim = claimAndSupportsToJSON(coinsCache, claimNsupports);
        claim.pushKV(T_BID, (int)i);
        claim.pushKV(T_SEQUENCE, (int)indexOf(seqOrder, claimNsupports.claim.claimId));
        claimObjs.push_back(claim);
    }

    UniValue unmatchedSupports(UniValue::VARR);
    for (auto& support : csToName.unmatchedSupports)
        unmatchedSupports.push_back(supportToJSON(coinsCache, support));

    result.pushKV(T_CLAIMS, claimObjs);
    result.pushKV(T_LASTTAKEOVERHEIGHT, csToName.nLastTakeoverHeight);
    result.pushKV(T_SUPPORTSWITHOUTCLAIM, unmatchedSupports);
    return result;
}

UniValue getclaimbybid(const JSONRPCRequest& request)
{
    validateRequest(request, GETCLAIMBYBID, 1, 2);

    LOCK(cs_main);
    CCoinsViewCache coinsCache(pcoinsTip.get());
    CClaimTrieCache trieCache(pclaimTrie);

    int bid = 0;
    if (request.params.size() > 1)
        bid = request.params[1].get_int();

    if (bid < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, T_BID " (parameter 2) should not be a negative value");

    if (request.params.size() > 2) {
        CBlockIndex* blockIndex = BlockHashIndex(ParseHashV(request.params[2], T_BLOCKHASH " (optional parameter 3)"));
        RollBackTo(blockIndex, coinsCache, trieCache);
    }

    std::string name = request.params[0].get_str();
    auto csToName = trieCache.getClaimsForName(name);

    UniValue result(UniValue::VOBJ);

    if (uint32_t(bid) >= csToName.claimsNsupports.size())
        return result;

    auto& claimNsupports = csToName.claimsNsupports[bid];
    std::size_t seq = 0;
    if (csToName.claimsNsupports.size() > 1) {
        auto seqOrder = seqSort(csToName.claimsNsupports);
        seq = indexOf(seqOrder, claimNsupports.claim.claimId);
    }

    result.pushKV(T_NORMALIZEDNAME, escapeNonUtf8(csToName.name));
    result.pushKVs(claimAndSupportsToJSON(coinsCache, claimNsupports));
    result.pushKV(T_LASTTAKEOVERHEIGHT, csToName.nLastTakeoverHeight);
    result.pushKV(T_BID, bid);
    result.pushKV(T_SEQUENCE, (int)seq);
    return result;
}

UniValue getclaimbyseq(const JSONRPCRequest& request)
{
    validateRequest(request, GETCLAIMBYSEQ, 1, 2);

    LOCK(cs_main);
    CCoinsViewCache coinsCache(pcoinsTip.get());
    CClaimTrieCache trieCache(pclaimTrie);

    int seq = 0;
    if (request.params.size() > 1)
        seq = request.params[1].get_int();

    if (seq < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, T_SEQUENCE " (parameter 2) should not be a negative value");

    if (request.params.size() > 2) {
        CBlockIndex* blockIndex = BlockHashIndex(ParseHashV(request.params[2], T_BLOCKHASH " (optional parameter 3)"));
        RollBackTo(blockIndex, coinsCache, trieCache);
    }

    std::string name = request.params[0].get_str();
    auto csToName = trieCache.getClaimsForName(name);

    UniValue result(UniValue::VOBJ);

    if (uint32_t(seq) >= csToName.claimsNsupports.size())
        return result;

    std::size_t bid = 0;
    const auto claimNsupports = [&bid, &seq, &csToName]() -> CClaimNsupports {
        if (csToName.claimsNsupports.size() == 1)
            return csToName.claimsNsupports[0];
        auto claimNsupports = seqSort(csToName.claimsNsupports)[seq];
        bid = indexOf(csToName.claimsNsupports, claimNsupports.claim.claimId);
        return claimNsupports;
    }();

    result.pushKV(T_NORMALIZEDNAME, escapeNonUtf8(csToName.name));
    result.pushKVs(claimAndSupportsToJSON(coinsCache, claimNsupports));
    result.pushKV(T_LASTTAKEOVERHEIGHT, csToName.nLastTakeoverHeight);
    result.pushKV(T_BID, (int)bid);
    result.pushKV(T_SEQUENCE, seq);
    return result;
}

UniValue getclaimbyid(const JSONRPCRequest& request)
{
    validateRequest(request, GETCLAIMBYID, 1, 0);

    LOCK(cs_main);
    CClaimTrieCache trieCache(pclaimTrie);
    CCoinsViewCache coinsCache(pcoinsTip.get());

    std::string claimId;
    ParseClaimtrieId(request.params[0], claimId, T_CLAIMID " (parameter 1)");

    if (claimId.length() < 3)
        throw JSONRPCError(RPC_INVALID_PARAMETER, T_CLAIMID " (parameter 1) should be at least 3 chars");

    std::string name;
    CClaimValue claim;
    UniValue ret(UniValue::VOBJ);
    bool found = claimId.length() == claimIdHexLength && getClaimById(uint160S(claimId), name, &claim);
    if (found || getClaimById(claimId, name, &claim)) {
        auto csToName = trieCache.getClaimsForName(name);
        auto& claimNsupports = csToName.find(claim.claimId);
        if (!claimNsupports.IsNull()) {
            std::size_t seq = 0, bid = 0;
            if (csToName.claimsNsupports.size() > 1) {
                auto seqOrder = seqSort(csToName.claimsNsupports);
                seq = indexOf(seqOrder, claim.claimId);
                bid = indexOf(csToName.claimsNsupports, claim.claimId);
            }
            ret.pushKV(T_NORMALIZEDNAME, escapeNonUtf8(csToName.name));
            ret.pushKVs(claimAndSupportsToJSON(coinsCache, claimNsupports));
            ret.pushKV(T_LASTTAKEOVERHEIGHT, csToName.nLastTakeoverHeight);
            ret.pushKV(T_BID, (int)bid);
            ret.pushKV(T_SEQUENCE, (int)seq);
        }
    }
    return ret;
}

UniValue gettotalclaimednames(const JSONRPCRequest& request)
{
    validateRequest(request, GETTOTALCLAIMEDNAMES, 0, 0);

    LOCK(cs_main);
    auto num_names = pclaimTrie->getTotalNamesInTrie();
    return int(num_names);
}

UniValue gettotalclaims(const JSONRPCRequest& request)
{
    validateRequest(request, GETTOTALCLAIMS, 0, 0);

    LOCK(cs_main);
    auto num_claims = pclaimTrie->getTotalClaimsInTrie();
    return int(num_claims);
}

UniValue gettotalvalueofclaims(const JSONRPCRequest& request)
{
    validateRequest(request, GETTOTALVALUEOFCLAIMS, 0, 1);

    LOCK(cs_main);
    bool controlling_only = false;
    if (request.params.size() == 1)
        controlling_only = request.params[0].get_bool();
    auto total_amount = pclaimTrie->getTotalValueOfClaimsInTrie(controlling_only);
    return ValueFromAmount(total_amount);
}

UniValue getclaimsfortx(const JSONRPCRequest& request)
{
    validateRequest(request, GETCLAIMSFORTX, 1, 0);

    LOCK(cs_main);
    uint256 hash = ParseHashV(request.params[0], T_TXID " (parameter 1)");
    UniValue ret(UniValue::VARR);

    int op;
    std::vector<std::vector<unsigned char> > vvchParams;

    CClaimTrieCache trieCache(pclaimTrie);
    CCoinsViewCache view(pcoinsTip.get());
    const Coin& coin = AccessByTxid(view, hash);
    std::vector<CTxOut> txouts{ coin.out };
    int nHeight = coin.nHeight;

    for (unsigned int i = 0; i < txouts.size(); ++i) {
        if (txouts[i].IsNull())
            continue;
        vvchParams.clear();
        const CTxOut& txout = txouts[i];
        if (!DecodeClaimScript(txout.scriptPubKey, op, vvchParams))
            continue;
        UniValue o(UniValue::VOBJ);
        o.pushKV(T_N, static_cast<int64_t>(i));
        std::string sName(vvchParams[0].begin(), vvchParams[0].end());
        o.pushKV(T_NAME, escapeNonUtf8(sName));
        if (op == OP_CLAIM_NAME) {
            uint160 claimId = ClaimIdHash(hash, i);
            o.pushKV(T_CLAIMID, claimId.GetHex());
            o.pushKV(T_VALUE, HexStr(vvchParams[1].begin(), vvchParams[1].end()));
        } else if (op == OP_UPDATE_CLAIM || op == OP_SUPPORT_CLAIM) {
            uint160 claimId(vvchParams[1]);
            o.pushKV(T_CLAIMID, claimId.GetHex());
            if (vvchParams.size() > 2)
                o.pushKV(T_VALUE, HexStr(vvchParams[2].begin(), vvchParams[2].end()));
        }
        if (nHeight > 0) {
            o.pushKV(T_DEPTH, chainActive.Height() - nHeight);
            if (op == OP_CLAIM_NAME || op == OP_UPDATE_CLAIM) {
                bool inClaimTrie = trieCache.haveClaim(sName, COutPoint(hash, i));
                o.pushKV(T_INCLAIMTRIE, inClaimTrie);
                if (inClaimTrie) {
                    CClaimValue claim;
                    if (!trieCache.getInfoForName(sName, claim))
                        LogPrintf("HaveClaim was true but getInfoForName returned false.");
                    o.pushKV(T_ISCONTROLLING, (claim.outPoint.hash == hash && claim.outPoint.n == i));
                } else {
                    int nValidAtHeight;
                    if (trieCache.haveClaimInQueue(sName, COutPoint(hash, i), nValidAtHeight)) {
                        o.pushKV(T_INQUEUE, true);
                        o.pushKV(T_BLOCKSTOVALID, nValidAtHeight - chainActive.Height());
                    } else
                        o.pushKV(T_INQUEUE, false);
                    }
            } else if (op == OP_SUPPORT_CLAIM) {
                bool inSupportMap = trieCache.haveSupport(sName, COutPoint(hash, i));
                o.pushKV(T_INSUPPORTMAP, inSupportMap);
                if (!inSupportMap) {
                    int nValidAtHeight;
                    if (trieCache.haveSupportInQueue(sName, COutPoint(hash, i), nValidAtHeight)) {
                        o.pushKV(T_INQUEUE, true);
                        o.pushKV(T_BLOCKSTOVALID, nValidAtHeight - chainActive.Height());
                    } else
                        o.pushKV(T_INQUEUE, false);
                }
            }
        } else {
            o.pushKV(T_DEPTH, 0);
            if (op == OP_CLAIM_NAME || op == OP_UPDATE_CLAIM)
                o.pushKV(T_INCLAIMTRIE, false);
            else if (op == OP_SUPPORT_CLAIM)
                o.pushKV(T_INSUPPORTMAP, false);
            o.pushKV(T_INQUEUE, false);
        }
        ret.push_back(o);
    }
    return ret;
}

UniValue proofToJSON(const CClaimTrieProof& proof)
{
    UniValue result(UniValue::VOBJ);
    UniValue nodes(UniValue::VARR);

    for (const auto& itNode : proof.nodes) {
        UniValue node(UniValue::VOBJ);
        UniValue children(UniValue::VARR);

        for (const auto& itChildren : itNode.children) {
            UniValue child(UniValue::VOBJ);
            child.pushKV(T_CHARACTER, itChildren.first);
            if (!itChildren.second.IsNull())
                child.pushKV(T_NODEHASH, itChildren.second.GetHex());
            children.push_back(child);
        }
        node.pushKV(T_CHILDREN, children);

        if (itNode.hasValue && !itNode.valHash.IsNull())
            node.pushKV(T_VALUEHASH, itNode.valHash.GetHex());
        nodes.push_back(node);
    }

    if (!nodes.empty())
        result.push_back(Pair(T_NODES, nodes));

    UniValue pairs(UniValue::VARR);

    for (const auto& itPair : proof.pairs) {
        UniValue child(UniValue::VOBJ);
        child.push_back(Pair(T_ODD, itPair.first));
        child.push_back(Pair(T_HASH, itPair.second.GetHex()));
        pairs.push_back(child);
    }

    if (!pairs.empty())
        result.push_back(Pair(T_PAIRS, pairs));

    if (proof.hasValue) {
        result.pushKV(T_TXID, proof.outPoint.hash.GetHex());
        result.pushKV(T_N, (int)proof.outPoint.n);
        result.pushKV(T_LASTTAKEOVERHEIGHT, (int)proof.nHeightOfLastTakeover);
    }
    return result;
}

UniValue getnameproof(const JSONRPCRequest& request)
{
    validateRequest(request, GETNAMEPROOF, 1, 2);

    LOCK(cs_main);
    CCoinsViewCache coinsCache(pcoinsTip.get());
    CClaimTrieCache trieCache(pclaimTrie);

    if (request.params.size() > 1) {
        CBlockIndex* pblockIndex = BlockHashIndex(ParseHashV(request.params[1], T_BLOCKHASH " (optional parameter 2)"));
        RollBackTo(pblockIndex, coinsCache, trieCache);
    }

    std::string claimId;
    if (request.params.size() > 2)
        ParseClaimtrieId(request.params[2], claimId, T_CLAIMID " (optional parameter 3)");

    std::function<bool(const CClaimValue&)> comp;
    if (claimId.length() == claimIdHexLength) {
        auto claimIdx = uint160S(claimId);
        comp = [claimIdx](const CClaimValue& claim) {
            return claim.claimId == claimIdx;
        };
    } else if (!claimId.empty()) {
        comp = [&claimId](const CClaimValue& claim) {
            return claim.claimId.GetHex().find(claimId) == 0;
        };
    }

    CClaimTrieProof proof;
    std::string name = request.params[0].get_str();
    if (!trieCache.getProofForName(name, proof, comp))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to generate proof");

    return proofToJSON(proof);
}

UniValue getclaimproofbybid(const JSONRPCRequest& request)
{
    validateRequest(request, GETCLAIMPROOFBYBID, 1, 2);

    LOCK(cs_main);
    CCoinsViewCache coinsCache(pcoinsTip.get());
    CClaimTrieCache trieCache(pclaimTrie);

    int bid = 0;
    if (request.params.size() > 1)
        bid = request.params[1].get_int();

    if (bid < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, T_BID " (parameter 2) should not be a negative value");

    if (request.params.size() > 2) {
        CBlockIndex* pblockIndex = BlockHashIndex(ParseHashV(request.params[2], T_BLOCKHASH " (optional parameter 3)"));
        RollBackTo(pblockIndex, coinsCache, trieCache);
    }

    std::string name = request.params[0].get_str();

    std::function<bool(const CClaimValue&)> comp;
    if (bid) {
        auto csToName = trieCache.getClaimsForName(name);
        if (uint32_t(bid) >= csToName.claimsNsupports.size())
            return {UniValue::VARR};
        auto claimId = csToName.claimsNsupports[bid].claim.claimId;
        comp = [claimId](const CClaimValue& claim) {
            return claim.claimId == claimId;
        };
    }

    CClaimTrieProof proof;
    if (!trieCache.getProofForName(name, proof, comp))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to generate proof");

    return proofToJSON(proof);
}

UniValue getclaimproofbyseq(const JSONRPCRequest& request)
{
    validateRequest(request, GETCLAIMPROOFBYSEQ, 1, 2);

    LOCK(cs_main);
    CCoinsViewCache coinsCache(pcoinsTip.get());
    CClaimTrieCache trieCache(pclaimTrie);

    int seq = 0;
    if (request.params.size() > 1)
        seq = request.params[1].get_int();

    if (seq < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, T_SEQUENCE " (parameter 2) should not be a negative value");

    if (request.params.size() > 2) {
        CBlockIndex* pblockIndex = BlockHashIndex(ParseHashV(request.params[2], T_BLOCKHASH " (optional parameter 3"));
        RollBackTo(pblockIndex, coinsCache, trieCache);
    }

    std::string name = request.params[0].get_str();
    auto csToName = trieCache.getClaimsForName(name);
    if (uint32_t(seq) >= csToName.claimsNsupports.size())
        return {UniValue::VARR};

    std::function<bool(const CClaimValue&)> comp;
    auto claimId = (csToName.claimsNsupports.size() == 1 ?
        csToName.claimsNsupports[0] : seqSort(csToName.claimsNsupports)[seq]).claim.claimId;
    comp = [&claimId](const CClaimValue& claim) {
        return claim.claimId == claimId;
    };

    CClaimTrieProof proof;
    if (!trieCache.getProofForName(name, proof, comp))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to generate proof");

    return proofToJSON(proof);
}

extern bool UndoReadFromDisk(CBlockUndo&, const CBlockIndex*);

template <typename T>
UniValue removedToJSON(const std::vector<queueEntryType<T>>& undo)
{
    UniValue ret(UniValue::VARR);
    for (auto& u : undo) {
        auto& outPoint = u.second.outPoint;
        ret.push_back(ClaimIdHash(outPoint.hash, outPoint.n).ToString());
    }
    return ret;
}

UniValue getchangesinblock(const JSONRPCRequest& request)
{
    validateRequest(request, GETCHANGESINBLOCK, 0, 1);

    CBlockUndo undo;
    {
        LOCK(cs_main);
        auto index = chainActive.Tip();
        if (request.params.size() > 0)
            index = BlockHashIndex(ParseHashV(request.params[0], T_BLOCKHASH " (optional parameter)"));

        if (!UndoReadFromDisk(undo, index))
            throw JSONRPCError(RPC_INTERNAL_ERROR,
                                   "Unable to read the undo block for height " + std::to_string(index->nHeight));
    }

    auto addedUpdated = [](const insertUndoType& insertUndo) {
        UniValue added(UniValue::VARR);
        for (auto& a : insertUndo)
            added.push_back(ClaimIdHash(a.outPoint.hash, a.outPoint.n).ToString());
        return added;
    };

    UniValue result(UniValue::VOBJ);
    result.pushKV(T_CLAIMSADDEDORUPDATED, addedUpdated(undo.insertUndo));
    result.pushKV(T_CLAIMSREMOVED, removedToJSON(undo.expireUndo));
    result.pushKV(T_SUPPORTSADDEDORUPDATED, addedUpdated(undo.insertSupportUndo));
    result.pushKV(T_SUPPORTSREMOVED, removedToJSON(undo.expireSupportUndo));
    return result;
}

UniValue checknormalization(const JSONRPCRequest& request)
{
    validateRequest(request, CHECKNORMALIZATION, 1, 0);

    const bool force = true;
    const std::string name = request.params[0].get_str();

    CClaimTrieCache triecache(pclaimTrie);
    return triecache.normalizeClaimName(name, force);
}

static const CRPCCommand commands[] =
{ //  category              name                            actor (function)            argNames
  //  --------------------- ------------------------        -----------------------     ----------
    { "Claimtrie",          "getclaimsintrie",              &getclaimsintrie,           { T_BLOCKHASH } },
    { "Claimtrie",          "getnamesintrie",               &getnamesintrie,            { T_BLOCKHASH } },
    { "hidden",             "getclaimtrie",                 &getclaimtrie,              { } },
    { "Claimtrie",          "getvalueforname",              &getvalueforname,           { T_NAME,T_BLOCKHASH,T_CLAIMID } },
    { "Claimtrie",          "getclaimsforname",             &getclaimsforname,          { T_NAME,T_BLOCKHASH } },
    { "Claimtrie",          "gettotalclaimednames",         &gettotalclaimednames,      { } },
    { "Claimtrie",          "gettotalclaims",               &gettotalclaims,            { } },
    { "Claimtrie",          "gettotalvalueofclaims",        &gettotalvalueofclaims,     { T_CONTROLLINGONLY } },
    { "Claimtrie",          "getclaimsfortx",               &getclaimsfortx,            { T_TXID } },
    { "Claimtrie",          "getnameproof",                 &getnameproof,              { T_NAME,T_BLOCKHASH,T_CLAIMID } },
    { "Claimtrie",          "getclaimproofbybid",           &getclaimproofbybid,        { T_NAME,T_BID,T_BLOCKHASH } },
    { "Claimtrie",          "getclaimproofbyseq",           &getclaimproofbyseq,        { T_NAME,T_SEQUENCE,T_BLOCKHASH } },
    { "Claimtrie",          "getclaimbyid",                 &getclaimbyid,              { T_CLAIMID } },
    { "Claimtrie",          "getclaimbybid",                &getclaimbybid,             { T_NAME,T_BID,T_BLOCKHASH } },
    { "Claimtrie",          "getclaimbyseq",                &getclaimbyseq,             { T_NAME,T_SEQUENCE,T_BLOCKHASH } },
    { "Claimtrie",          "getchangesinblock",            &getchangesinblock,         { T_BLOCKHASH } },
    { "Claimtrie",          "checknormalization",           &checknormalization,        { T_NAME } },
};

void RegisterClaimTrieRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
