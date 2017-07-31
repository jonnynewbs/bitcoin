#include <interface/chain.h>
#include <interface/handler.h>
#include <interface/util.h>
#include <wallet/init.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

class CScheduler;

namespace interface {
namespace {

class WalletClientImpl : public Chain::Client
{
public:
    WalletClientImpl(Chain& chain, std::vector<std::string> wallet_filenames)
        : m_chain(chain), m_wallet_filenames(std::move(wallet_filenames))
    {
    }
    void registerRpcs() override { RegisterWalletRPCCommands(m_chain, m_rpc_handlers); }
    bool prepare() override { return LoadWallets(m_chain, *this, m_wallet_filenames); }
    void start(CScheduler& scheduler) override { StartWallets(scheduler); }
    void stop() override { FlushWallets(); }
    void shutdown() override
    {
        for (CWalletRef wallet : ::vpwallets) {
            wallet->Flush(true /* shutdown */);
        }
    }
    ~WalletClientImpl() override { CloseWallets(); }

    Chain& m_chain;
    std::vector<std::string> m_wallet_filenames;
    std::vector<std::unique_ptr<Handler>> m_rpc_handlers;
};

} // namespace

std::unique_ptr<Chain::Client> MakeWalletClient(Chain& chain, std::vector<std::string> wallet_filenames)
{
    return MakeUnique<WalletClientImpl>(chain, std::move(wallet_filenames));
}

} // namespace interface
