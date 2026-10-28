/*!
 * \file r164_test_util.h
 * \brief R164 lane（第二十四波 · WtDtCore 剩余面）自用工具，仅 r164_*.cpp 使用。
 *
 * 约束：
 *  - 临时文件只落系统临时目录，目录名带 r164_ + pid + 序号，fixture 负责删除；
 *  - UDP 只 bind 127.0.0.1 / 0.0.0.0 的临时端口，客户端只向 127.0.0.1 发包；
 *  - LeakyCaster 故意不 delete：UDPCaster 的成员析构顺序会让 io_context 先于 socket
 *    销毁，active 装配后析构必 SIGSEGV（BUG-R164-001）。用例进程由 case_runner
 *    单独拉起，进程退出即回收；这里只保证 stop() 把产品线程 join 干净。
 */
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/StateMonitor.h>
#include <WtDtCore/UDPCaster.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <TimeUtils.hpp>

namespace r164 {

namespace fs = std::filesystem;

inline fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r164_") + tag + "_" + std::to_string(::getpid()) + "_"
			+ std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

inline void write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

inline WTSVariant* load_cfg(const fs::path& dir, const char* name, const std::string& body)
{
	fs::path p = dir / name;
	write_file(p, body);
	return WTSCfgLoader::load_from_file(p.string().c_str());
}

inline bool wait_until(const std::function<bool()>& pred, int timeout_ms)
{
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (pred())
			return true;

		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return pred();
}

inline bool wait_state(StateMonitor& mon, const char* sid, SimpleState ss,
	int timeout_ms = 6000)
{
	return wait_until([&mon, sid, ss]() { return mon.isInState(sid, ss); }, timeout_ms);
}

inline uint32_t current_minute()
{
	return TimeUtils::getCurMin() / 100;
}

inline bool is_weekend()
{
	const uint32_t wd = TimeUtils::getWeekDay();
	return wd == 0 || wd == 6;
}

/* UDPCaster.cpp:30-34 的请求包线格式；只用于回环报文。 */
#pragma pack(push, 1)
struct ReqPacket
{
	uint32_t _type;
	char _data[1020];
};
#pragma pack(pop)

static_assert(sizeof(ReqPacket) == 1024,
	"UDPCaster::do_receive 只把 1024 字节的报文当 UDPReqPacket");

inline const char* cannot_identify_text()
{
	return "Can not indentify the command";
}

inline int bind_udp_loopback(uint16_t* boundPort)
{
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
	{
		::close(fd);
		return -1;
	}

	socklen_t len = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
	{
		::close(fd);
		return -1;
	}

	if (boundPort != nullptr)
		*boundPort = ntohs(addr.sin_port);
	return fd;
}

inline int pick_free_udp_port()
{
	// 与 UDPCaster::start() 的 bind 形态一致（0.0.0.0），避免探测端口与
	// 实际 bind 的通配地址语义不同。
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return 0;

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = 0;

	if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
	{
		::close(fd);
		return 0;
	}

	socklen_t len = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
	{
		::close(fd);
		return 0;
	}

	const uint16_t port = ntohs(addr.sin_port);
	::close(fd);
	return port;
}

inline bool send_datagram(int fd, uint16_t port, const void* data, std::size_t len)
{
	sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	dst.sin_port = htons(port);

	const ssize_t n = ::sendto(fd, data, len, 0,
		reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
	return n == static_cast<ssize_t>(len);
}

inline int recv_datagram(int fd, std::vector<char>& buf, int timeoutMs)
{
	pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	const int pr = ::poll(&pfd, 1, timeoutMs);
	if (pr <= 0)
		return -1;

	buf.resize(2048);
	const ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), 0, nullptr, nullptr);
	if (n < 0)
		return -1;

	buf.resize(static_cast<std::size_t>(n));
	return static_cast<int>(n);
}

/*
 * UDPCaster 析构时按成员声明逆序执行：m_ioservice（UDPCaster.h:96）先于
 * m_sktBroadcast/m_sktSubscribe/m_list*Group（UDPCaster.h:88-95）销毁，
 * socket 析构访问已析构的 reactor，SIGSEGV。BUG-R164-001。
 * 用例需要真实 socket 时只能不 delete；stop_once() 负责 join 产品线程。
 */
class LeakyCaster
{
public:
	LeakyCaster() : _caster(new UDPCaster()) {}
	~LeakyCaster() { stop_once(); }

	LeakyCaster(const LeakyCaster&) = delete;
	LeakyCaster& operator=(const LeakyCaster&) = delete;

	UDPCaster* operator->() const { return _caster; }
	UDPCaster& get() const { return *_caster; }

	void arm() { _armed = true; }

	void stop_once()
	{
		if (_armed)
		{
			_armed = false;
			_caster->stop();
		}
	}

private:
	UDPCaster* _caster;
	bool _armed { false };
};

/* stop() 非幂等（对已 join 的线程再次 join 抛 system_error），用 armed 保证只 join 一次。 */
class MonitorGuard
{
public:
	explicit MonitorGuard(StateMonitor& mon) : _mon(mon) {}
	~MonitorGuard() { stop(); }

	void arm() { _armed = true; }

	void stop()
	{
		if (_armed)
		{
			_armed = false;
			_mon.stop();
		}
	}

private:
	StateMonitor& _mon;
	bool _armed { false };
};

}	// namespace r164
