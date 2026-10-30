#include <Includes/ITraderApi.h>
#include <Includes/WTSVariant.hpp>

#include <fstream>

#if defined(_WIN32)
#define R59_CREDENTIALS_EXPORT __declspec(dllexport)
#else
#define R59_CREDENTIALS_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
class CredentialsTrader final : public wtp::ITraderApi
{
public:
	bool init(wtp::WTSVariant* params) override
	{
		if (params == nullptr)
			return false;
		const char* path = params->getCString("r59_login_record");
		if (path == nullptr || *path == '\0')
			return false;
		_record_path = path;
		return true;
	}

	void registerSpi(wtp::ITraderSpi* spi) override
	{
		_spi = spi;
	}

	void connect() override
	{
		if (_spi != nullptr)
			_spi->handleEvent(wtp::WTE_Connect, 0);
	}

	int login(const char* user, const char* pass, const char* /*productInfo*/) override
	{
		if (_record_path.empty())
			return -1;

		std::ofstream out(_record_path, std::ios::binary | std::ios::app);
		if (!out.is_open())
			return -1;

		out << (user == nullptr ? "<null>" : user) << '\n'
			<< (pass == nullptr ? "<null>" : pass) << '\n';
		return out.good() ? 0 : -1;
	}

private:
	wtp::ITraderSpi* _spi = nullptr;
	std::string _record_path;
};
}

extern "C" R59_CREDENTIALS_EXPORT wtp::ITraderApi* createTrader()
{
	return new CredentialsTrader();
}

extern "C" R59_CREDENTIALS_EXPORT void deleteTrader(wtp::ITraderApi*& trader)
{
	delete trader;
	trader = nullptr;
}
