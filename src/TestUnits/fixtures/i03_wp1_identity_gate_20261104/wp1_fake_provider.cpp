// Loadable fake trader plugin for the WP1 identity/gate fixture. It never
// connects anywhere: every call is delegated to an in-process backend object
// that the test registers through wp1PushBackend() before TraderAdapter::init.
#include <Includes/ITraderApi.h>
#include <deque>

namespace {
std::deque<wtp::ITraderApi*> g_backends;

class Proxy final : public wtp::ITraderApi
{
public:
    explicit Proxy(wtp::ITraderApi* backend) : b(backend) {}
    bool init(wtp::WTSVariant* params) override { return b->init(params); }
    void release() override { b->release(); }
    void registerSpi(wtp::ITraderSpi* listener) override { b->registerSpi(listener); }
    void connect() override { b->connect(); }
    void disconnect() override { b->disconnect(); }
    bool isConnected() override { return b->isConnected(); }
    bool makeEntrustID(char* buffer, int length) override { return b->makeEntrustID(buffer, length); }
    int login(const char* user, const char* pass, const char* info) override { return b->login(user, pass, info); }
    int logout() override { return b->logout(); }
    int orderInsert(wtp::WTSEntrust* entrust) override { return b->orderInsert(entrust); }
    int orderAction(wtp::WTSEntrustAction* action) override { return b->orderAction(action); }
    int queryAccount() override { return b->queryAccount(); }
    int queryPositions() override { return b->queryPositions(); }
    int queryOrders() override { return b->queryOrders(); }
    int queryTrades() override { return b->queryTrades(); }
    int querySettlement(uint32_t date) override { return b->querySettlement(date); }

private:
    wtp::ITraderApi* b;
};
}

extern "C" __attribute__((visibility("default"))) void wp1PushBackend(wtp::ITraderApi* backend)
{
    g_backends.push_back(backend);
}

extern "C" __attribute__((visibility("default"))) wtp::ITraderApi* createTrader()
{
    if (g_backends.empty())
        return nullptr;
    wtp::ITraderApi* backend = g_backends.front();
    g_backends.pop_front();
    return new Proxy(backend);
}

extern "C" __attribute__((visibility("default"))) void deleteTrader(wtp::ITraderApi*& trader)
{
    delete trader;
    trader = nullptr;
}
