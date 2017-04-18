#include <interface/node.h>

#include <addrdb.h>
#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <init.h>
#include <interface/handler.h>
#include <interface/util.h>
#include <interface/wallet.h>
#include <net.h>
#include <net_processing.h>
#include <netaddress.h>
#include <netbase.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <rpc/server.h>
#include <scheduler.h>
#include <sync.h>
#include <txmempool.h>
#include <ui_interface.h>
#include <util.h>
#include <validation.h>
#include <warnings.h>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif
#ifdef ENABLE_WALLET
#include <wallet/fees.h>
#include <wallet/wallet.h>
#define CHECK_WALLET(x) x
#else
#define CHECK_WALLET(x) throw std::logic_error("Wallet function called in non-wallet build.")
#endif

#include <atomic>
#include <boost/thread/thread.hpp>
#include <univalue.h>

namespace interface {
namespace {

class NodeImpl : public Node
{
public:
    void parseParameters(int argc, const char* const argv[]) override { gArgs.ParseParameters(argc, argv); }
    void readConfigFile(const std::string& conf_path) override { gArgs.ReadConfigFile(conf_path); }
    bool softSetArg(const std::string& arg, const std::string& value) override { return gArgs.SoftSetArg(arg, value); }
    bool softSetBoolArg(const std::string& arg, bool value) override { return gArgs.SoftSetBoolArg(arg, value); }
    void selectParams(const std::string& network) override { SelectParams(network); }
    std::string getNetwork() override { return Params().NetworkIDString(); }
    void initLogging() override { InitLogging(); }
    void initParameterInteraction() override { InitParameterInteraction(); }
    std::string getWarnings(const std::string& type) override { return GetWarnings(type); }
    bool baseInitialize() override
    {

        return AppInitBasicSetup() && AppInitParameterInteraction() && AppInitSanityChecks() &&
               AppInitLockDataDirectory();
    }
    bool appInitMain() override { return AppInitMain(m_thread_group, m_scheduler); }
    void appShutdown() override
    {
        Interrupt(m_thread_group);
        m_thread_group.join_all();
        Shutdown();
    }
    void startShutdown() override { StartShutdown(); }
    bool shutdownRequested() override { return ShutdownRequested(); }
    std::string helpMessage(HelpMessageMode mode) override { return HelpMessage(mode); }
    void mapPort(bool use_upnp) override { MapPort(use_upnp); }
    bool getProxy(Network net, proxyType& proxy_info) override { return GetProxy(net, proxy_info); }
    size_t getNodeCount(CConnman::NumConnections flags) override
    {
        return g_connman ? g_connman->GetNodeCount(flags) : 0;
    }
    bool getNodesStats(NodesStats& stats) override
    {
        stats.clear();

        if (g_connman) {
            std::vector<CNodeStats> stats_temp;
            g_connman->GetNodeStats(stats_temp);

            stats.reserve(stats_temp.size());
            for (auto& node_stats_temp : stats_temp) {
                stats.emplace_back(std::move(node_stats_temp), false, CNodeStateStats());
            }

            // Try to retrieve the CNodeStateStats for each node.
            TRY_LOCK(::cs_main, lockMain);
            if (lockMain) {
                for (auto& node_stats : stats) {
                    std::get<1>(node_stats) =
                        GetNodeStateStats(std::get<0>(node_stats).nodeid, std::get<2>(node_stats));
                }
            }
            return true;
        }
        return false;
    }
    bool getBanned(banmap_t& banmap) override
    {
        if (g_connman) {
            g_connman->GetBanned(banmap);
            return true;
        }
        return false;
    }
    bool ban(const CNetAddr& net_addr, BanReason reason, int64_t ban_time_offset) override
    {
        if (g_connman) {
            g_connman->Ban(net_addr, reason, ban_time_offset);
            return true;
        }
        return false;
    }
    bool unban(const CSubNet& ip) override
    {
        if (g_connman) {
            g_connman->Unban(ip);
            return true;
        }
        return false;
    }
    bool disconnect(NodeId id) override
    {
        if (g_connman) {
            return g_connman->DisconnectNode(id);
        }
        return false;
    }
    int64_t getTotalBytesRecv() override { return g_connman ? g_connman->GetTotalBytesRecv() : 0; }
    int64_t getTotalBytesSent() override { return g_connman ? g_connman->GetTotalBytesSent() : 0; }
    size_t getMempoolSize() override { return ::mempool.size(); }
    size_t getMempoolDynamicUsage() override { return ::mempool.DynamicMemoryUsage(); }
    bool getHeaderTip(int& height, int64_t& block_time) override
    {
        LOCK(::cs_main);
        if (::pindexBestHeader) {
            height = ::pindexBestHeader->nHeight;
            block_time = ::pindexBestHeader->GetBlockTime();
            return true;
        }
        return false;
    }
    int getNumBlocks() override
    {
        LOCK(::cs_main);
        return ::chainActive.Height();
    }
    int64_t getLastBlockTime() override
    {
        LOCK(::cs_main);
        if (::chainActive.Tip()) {
            return ::chainActive.Tip()->GetBlockTime();
        }
        return Params().GenesisBlock().GetBlockTime(); // Genesis block's time of current network
    }
    double getVerificationProgress() override
    {
        const CBlockIndex* tip;
        {
            LOCK(::cs_main);
            tip = ::chainActive.Tip();
        }
        return GuessVerificationProgress(Params().TxData(), tip);
    }
    bool isInitialBlockDownload() override { return IsInitialBlockDownload(); }
    bool getReindex() override { return ::fReindex; }
    bool getImporting() override { return ::fImporting; }
    void setNetworkActive(bool active) override
    {
        if (g_connman) {
            g_connman->SetNetworkActive(active);
        }
    }
    bool getNetworkActive() override { return g_connman && g_connman->GetNetworkActive(); }
    unsigned int getTxConfirmTarget() override { CHECK_WALLET(return ::nTxConfirmTarget); }
    bool getWalletRbf() override { CHECK_WALLET(return ::fWalletRbf); }
    CAmount getRequiredFee(unsigned int tx_bytes) override { CHECK_WALLET(return GetRequiredFee(tx_bytes)); }
    CAmount getMinimumFee(unsigned int tx_bytes,
        const CCoinControl& coin_control,
        int* returned_target,
        FeeReason* reason) override
    {
        FeeCalculation fee_calc;
        CHECK_WALLET(return GetMinimumFee(tx_bytes, coin_control, ::mempool, ::feeEstimator, &fee_calc));
        if (returned_target) *returned_target = fee_calc.returnedTarget;
        if (reason) *reason = fee_calc.reason;
    }
    CAmount getMaxTxFee() override { return ::maxTxFee; }
    CFeeRate estimateSmartFee(int num_blocks, bool conservative, int* returned_target = nullptr) override
    {
        FeeCalculation fee_calc;
        CFeeRate result = ::feeEstimator.estimateSmartFee(num_blocks, &fee_calc, conservative);
        if (returned_target) {
            *returned_target = fee_calc.returnedTarget;
        }
        return result;
    }
    CFeeRate getDustRelayFee() override { return ::dustRelayFee; }
    CFeeRate getPayTxFee() override { CHECK_WALLET(return ::payTxFee); }
    UniValue executeRpc(const std::string& command, const UniValue& params, const std::string& uri) override
    {
        JSONRPCRequest req;
        req.params = params;
        req.strMethod = command;
        req.URI = uri;
        return ::tableRPC.execute(req);
    }
    std::vector<std::string> listRpcCommands() override { return ::tableRPC.listCommands(); }
    void rpcSetTimerInterfaceIfUnset(RPCTimerInterface* iface) override { RPCSetTimerInterfaceIfUnset(iface); }
    void rpcUnsetTimerInterface(RPCTimerInterface* iface) override { RPCUnsetTimerInterface(iface); }
    std::unique_ptr<Wallet> getWallet(size_t index) override
    {
        CHECK_WALLET(return index < ::vpwallets.size() ? MakeWallet(*::vpwallets[index]) : nullptr);
    }
    std::unique_ptr<Handler> handleInitMessage(InitMessageFn fn) override
    {
        return MakeHandler(::uiInterface.InitMessage.connect(fn));
    }
    std::unique_ptr<Handler> handleMessageBox(MessageBoxFn fn) override
    {
        return MakeHandler(::uiInterface.ThreadSafeMessageBox.connect(fn));
    }
    std::unique_ptr<Handler> handleQuestion(QuestionFn fn) override
    {
        return MakeHandler(::uiInterface.ThreadSafeQuestion.connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeHandler(::uiInterface.ShowProgress.connect(fn));
    }
    std::unique_ptr<Handler> handleLoadWallet(LoadWalletFn fn) override
    {
        CHECK_WALLET(
            return MakeHandler(::uiInterface.LoadWallet.connect([fn](CWallet* wallet) { fn(MakeWallet(*wallet)); })));
    }
    std::unique_ptr<Handler> handleNotifyNumConnectionsChanged(NotifyNumConnectionsChangedFn fn) override
    {
        return MakeHandler(::uiInterface.NotifyNumConnectionsChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyNetworkActiveChanged(NotifyNetworkActiveChangedFn fn) override
    {
        return MakeHandler(::uiInterface.NotifyNetworkActiveChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyAlertChanged(NotifyAlertChangedFn fn) override
    {
        return MakeHandler(::uiInterface.NotifyAlertChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleBannedListChanged(BannedListChangedFn fn) override
    {
        return MakeHandler(::uiInterface.BannedListChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyBlockTip(NotifyBlockTipFn fn) override
    {
        return MakeHandler(::uiInterface.NotifyBlockTip.connect([fn](bool initial_download, const CBlockIndex* block) {
            fn(initial_download, block->nHeight, block->GetBlockTime(),
                GuessVerificationProgress(Params().TxData(), block));
        }));
    }
    std::unique_ptr<Handler> handleNotifyHeaderTip(NotifyHeaderTipFn fn) override
    {
        return MakeHandler(
            ::uiInterface.NotifyHeaderTip.connect([fn](bool initial_download, const CBlockIndex* block) {
                fn(initial_download, block->nHeight, block->GetBlockTime(),
                    GuessVerificationProgress(Params().TxData(), block));
            }));
    }

    boost::thread_group m_thread_group;
    ::CScheduler m_scheduler;
};

} // namespace

std::unique_ptr<Node> MakeNode() { return MakeUnique<NodeImpl>(); }

} // namespace interface
