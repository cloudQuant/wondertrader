/*!
 * \file r154_udp_caster_start_test.cpp
 * \brief UDPCaster 的 active 装配、广播队列与回环收包（迭代01 第二十三波 R154）
 *
 * 与既有用例的分工（只读，不修改）：
 *  - r5/r68 已覆盖 active=false、地址解析失败、接收方分桶、未 start 的 broadcast 守卫；
 *  - 本文件只补 active=true 之后真实要走的三段：
 *      UDPCaster.cpp:62-143（init/start/stop 的 socket 与线程外壳）
 *      UDPCaster.cpp:288-400（do_broadcast 组包、raw 单播发送、CastData 引用计数）
 *      UDPCaster.cpp:145-220（do_receive 回调、订阅解析、错误回包）
 *
 * 网络约束：只 bind 127.0.0.1 的回环端口，不访问外网；配置里的组播地址只用于
 * addMRecver 的 join_group 分支，do_broadcast 从不向组播列表发送。所有线程都在
 * 用例作用域内 join；active 用例用 LeakyCaster 规避 ~UDPCaster 的成员析构顺序
 * 缺陷（BUG-R164-001），只 stop 不 delete，进程退出即回收。
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/UDPCaster.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include "r154_test_util.h"

namespace fs = std::filesystem;

namespace {

#pragma pack(push, 1)
/* UDPCaster.cpp:30-34 的 UDPReqPacket 线格式；只做回环报文，不触碰私有类型。 */
struct R154ReqPacket
{
	uint32_t _type;
	char _data[1020];
};
#pragma pack(pop)

static_assert(sizeof(R154ReqPacket) == 1024,
	"UDPCaster::do_receive 只接受 sizeof(UDPReqPacket)==1024 的请求包");

int bind_udp_loopback(uint16_t* boundPort)
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

int pick_free_udp_port()
{
	// 与 UDPCaster::start() 的 bind 形态保持一致（0.0.0.0），避免探测端口与
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

bool send_datagram(int fd, uint16_t port, const void* data, std::size_t len)
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

int recv_datagram(int fd, std::vector<char>& buf, int timeoutMs)
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

class R154UdpCasterTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r154::make_tmp_dir("udp"); }

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		fs::path p = _dir / name;
		r154::write_file(p, body);
		return WTSCfgLoader::load_from_file(p.string().c_str());
	}

	fs::path _dir;
};

/*
 * UDPCaster.cpp:62-143
 * active=true、没有任何 broadcast 接收方：start() 只建订阅 socket（sport=0 回落到
 * bport=0 的临时端口）并起 io 线程；do_receive 挂起 async_receive_from。
 * m_sktBroadcast 仍为 NULL，四个 broadcast 必须在入队前被守卫拦下。
 * stop() 停 io_context 并 join m_thrdIO（L137-138）。
 */
TEST_F(R154UdpCasterTest, InitActiveWithoutReceiversStartsSubscribeThenStops)
{
	WTSVariant* cfg = loadCfg("no_receivers.json", R"JSON({
		"active": true,
		"sport": 0,
		"bport": 0,
		"multicast": [
			{"host": "bad.host.name", "port": 9000, "sendport": 0, "type": 0}
		]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	r154::LeakyCaster caster;
	ASSERT_TRUE(caster->init(cfg, nullptr, nullptr));
	cfg->release();
	caster.arm();

	// 三个合法组地址的 addMRecver 分桶（join_group 成功与否取决于本机网卡，
	// 此处不向组播发送任何数据，只把成功/失败两条路径都留给覆盖率统计）。
	caster->addMRecver("239.255.0.10", 9010, 0, 0);
	caster->addMRecver("239.255.0.11", 9011, 0, 1);
	caster->addMRecver("239.255.0.12", 9012, 0, 2);

	WTSTickData* tick = WTSTickData::create("600000");
	ASSERT_NE(tick, nullptr);
	caster->broadcast(tick);
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
	caster.stop_once();
}

/*
 * UDPCaster.cpp:62-99、101-131、133-143、268-400
 * active=true + broadcast 数组（raw 127.0.0.1 真实接收方 + 两个不被 do_broadcast
 * 读取的 flat/json 桶 + 一个非法地址）与 multicast 数组（非法地址 + 合法组地址的
 * type=1 桶）。四个 broadcast 进入队列后由 m_thrdCast 组包并逐条 send_to：
 *  - 四个 dataType 分支（0x200/0x201/0x202/0x203）各走一次；
 *  - CastData 的 retain/copy/release（UDPCaster.h:107-134）在入队与出队时执行；
 *  - stop() 同时 join m_thrdIO（L138）和 m_thrdCast（L142）。
 * 组播列表只 join、不发送，因此没有出网流量。
 */
TEST_F(R154UdpCasterTest, InitActiveWithReceiversBroadcastsAllFourTypes)
{
	uint16_t recvPort = 0;
	const int recvFd = bind_udp_loopback(&recvPort);
	ASSERT_GE(recvFd, 0);

	const std::string body = std::string(R"JSON({
		"active": true,
		"sport": 0,
		"bport": 0,
		"broadcast": [
			{"host": "127.0.0.1", "port": )JSON") + std::to_string(recvPort) + R"JSON(, "type": 2},
			{"host": "127.0.0.1", "port": )JSON" + std::to_string(recvPort) + R"JSON(, "type": 1},
			{"host": "127.0.0.1", "port": )JSON" + std::to_string(recvPort) + R"JSON(, "type": 0},
			{"host": "not-an-ip", "port": )JSON" + std::to_string(recvPort) + R"JSON(, "type": 2}
		],
		"multicast": [
			{"host": "bad.host.name", "port": 9001, "sendport": 0, "type": 0},
			{"host": "239.255.0.9", "port": 9002, "sendport": 0, "type": 1}
		]
	})JSON";
	WTSVariant* cfg = loadCfg("broadcast_all.json", body);
	ASSERT_NE(cfg, nullptr);

	r154::LeakyCaster caster;
	ASSERT_TRUE(caster->init(cfg, nullptr, nullptr));
	cfg->release();
	caster.arm();

	WTSTickData* tick = WTSTickData::create("600000");
	WTSOrdQueData* que = WTSOrdQueData::create("600000");
	WTSOrdDtlData* dtl = WTSOrdDtlData::create("600000");
	WTSTransData* trans = WTSTransData::create("600000");
	ASSERT_NE(tick, nullptr);
	ASSERT_NE(que, nullptr);
	ASSERT_NE(dtl, nullptr);
	ASSERT_NE(trans, nullptr);

	caster->broadcast(tick);
	caster->broadcast(que);
	caster->broadcast(dtl);
	caster->broadcast(trans);

	// 队列里每条 CastData 都 retain 一次；全部出队后引用计数回到 1。
	ASSERT_TRUE(r154::wait_until([&]() {
		return tick->retainCount() == 1u && que->retainCount() == 1u
			&& dtl->retainCount() == 1u && trans->retainCount() == 1u;
	}, 3000)) << "broadcast 队列未被消费完";

	// raw 单播接收方应依次收到 4 个数据报；包长与包头类型来自 do_broadcast 的组包分支。
	const std::size_t expectLen[4] = {
		4u + sizeof(WTSTickStruct),
		4u + sizeof(WTSOrdQueStruct),
		4u + sizeof(WTSOrdDtlStruct),
		4u + sizeof(WTSTransStruct)
	};
	const uint32_t expectType[4] = { 0x200u, 0x201u, 0x202u, 0x203u };
	for (int i = 0; i < 4; i++)
	{
		std::vector<char> pkt;
		const int n = recv_datagram(recvFd, pkt, 3000);
		ASSERT_EQ(n, static_cast<int>(expectLen[i])) << "第 " << i << " 个数据报长度不符";
		uint32_t type = 0;
		memcpy(&type, pkt.data(), sizeof(type));
		EXPECT_EQ(type, expectType[i]);
	}

	// socket 非空、数据为空：do_broadcast 的 data==NULL 分支（L290）。
	caster->broadcast(static_cast<WTSTickData*>(nullptr));

	// 留出一个调度窗口让 m_thrdCast 进入 cond.wait，再 stop 的 notify_all 才能
	// 唤醒它退出；否则产品 stop() 的“置位 -> notify”存在丢失唤醒窗口（R5-007）。
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	// 终止后 socket 仍非空，m_bTerminated 分支拦下新的广播（L290 第三个条件）。
	caster.stop_once();
	caster->broadcast(tick);
	EXPECT_EQ(tick->retainCount(), 1u);

	::close(recvFd);

	tick->release();
	que->release();
	dtl->release();
	trans->release();
}

/*
 * UDPCaster.cpp:145-220
 * 回环发送 3 类请求，驱动 do_receive 的 async_receive_from 回调：
 *  1) 4 字节短包 -> L203-216 回 "Can not indentify the command"（含发送 lambda）；
 *  2) 1024 字节但 _type!=0x100 -> L162 条件为 false，只 re-arm；
 *  3) 1024 字节订阅包（含带点/不带点代码、有效合约、未知合约）-> L162-200 的分割、
 *     getContract、getCurTick(NULL) 与 continue 路径；DataManager 无 writer，
 *     L180-182 的 tick==NULL 分支必然命中，不会进入回包段。
 * 每个“无回包”的请求后追加一个短包作为屏障，收到第二个错误回包即证明前序请求
 * 已按序处理完毕（同一回环 socket 上的报文顺序处理）。
 */
TEST_F(R154UdpCasterTest, ReceiveMalformedAndSubscribeDatagramsOnLoopback)
{
	r154::write_file(_dir / "sessions.json", R"JSON({
		"DAY": {"name": "日盘", "offset": 0, "sections": [{"from": 930, "to": 1500}]}
	})JSON");
	r154::write_file(_dir / "commodities.json", R"JSON({
		"SHFE": {"ag": {"name": "白银", "session": "DAY", "holiday": "R154_NONE"}}
	})JSON");
	r154::write_file(_dir / "contracts.json", R"JSON({
		"SHFE": {"ag2512": {"name": "ag2512", "exchg": "SHFE", "product": "ag"}}
	})JSON");

	WTSBaseDataMgr bdMgr;
	ASSERT_TRUE(bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadContracts((_dir / "contracts.json").string().c_str()));
	DataManager dtMgr;

	const int sport = pick_free_udp_port();
	ASSERT_NE(sport, 0);

	WTSVariant* cfg = loadCfg("receive.json",
		std::string(R"JSON({"active": true, "sport": )JSON")
		+ std::to_string(sport) + R"JSON(})JSON");
	ASSERT_NE(cfg, nullptr);

	r154::LeakyCaster caster;
	ASSERT_TRUE(caster->init(cfg, &bdMgr, &dtMgr));
	cfg->release();
	caster.arm();

	const int clientFd = bind_udp_loopback(nullptr);
	ASSERT_GE(clientFd, 0);

	std::vector<char> reply;
	const char* expectText = "Can not indentify the command";

	// 1) 短包：长度不等于 sizeof(UDPReqPacket)，走错误回包。
	const uint32_t junk = 0x12345678u;
	ASSERT_TRUE(send_datagram(clientFd, static_cast<uint16_t>(sport), &junk, sizeof(junk)));
	ASSERT_GT(recv_datagram(clientFd, reply, 3000), 0);
	EXPECT_EQ(std::string(reply.begin(), reply.end()), expectText);

	// 2) 1024 字节但非订阅类型；随后用短包做屏障。
	R154ReqPacket unknown;
	memset(&unknown, 0, sizeof(unknown));
	unknown._type = 0x999u;
	ASSERT_TRUE(send_datagram(clientFd, static_cast<uint16_t>(sport), &unknown, sizeof(unknown)));
	ASSERT_TRUE(send_datagram(clientFd, static_cast<uint16_t>(sport), &junk, sizeof(junk)));
	ASSERT_GT(recv_datagram(clientFd, reply, 3000), 0);
	EXPECT_EQ(std::string(reply.begin(), reply.end()), expectText);

	// 3) 订阅包：不带点的代码命中空 exchg 查询，带点的命中 exchg 查询，
	//    未知合约在 getContract 返回 NULL 后 continue；有效合约在 getCurTick
	//    返回 NULL 后 continue。最后再发短包做屏障。
	R154ReqPacket sub;
	memset(&sub, 0, sizeof(sub));
	sub._type = 0x100u;
	const char* codes = "ag2512,SHFE.ag2512,NO.NOPE";
	memcpy(sub._data, codes, strlen(codes));
	ASSERT_TRUE(send_datagram(clientFd, static_cast<uint16_t>(sport), &sub, sizeof(sub)));
	ASSERT_TRUE(send_datagram(clientFd, static_cast<uint16_t>(sport), &junk, sizeof(junk)));
	ASSERT_GT(recv_datagram(clientFd, reply, 3000), 0);
	EXPECT_EQ(std::string(reply.begin(), reply.end()), expectText);

	caster.stop_once();
	::close(clientFd);
}

/*
 * 依赖前置条件未满足：QuoteFactory 的正常装配传入有效的 base/data manager；
 * 没有证据表明 active=true 时这两个依赖可省略。该探针不作为 BUG-R154-001 的
 * 产品复现或验收。本用例保持禁用，显式跳过，避免误用 --gtest_also_run_disabled_tests
 * 时触发空指针崩溃。
 */
TEST_F(R154UdpCasterTest, DISABLED_NullManagersAreAnInvalidDependencyProbe)
{
	GTEST_SKIP() << "active UDPCaster requires the managers supplied by QuoteFactory; "
				 << "null-manager behavior has no supported dependency contract.";
}

/*
 * 触发条件（禁用原因）：UDPCaster 的成员声明顺序（UDPCaster.h:88-96）使
 * m_ioservice（H96）先于 m_sktBroadcast/m_sktSubscribe/m_list*Group（H88-95）
 * 析构；active init 建过 socket 后对象离开作用域，socket 析构访问已销毁的
 * reactor，SIGSEGV。与 BUG-R164-001 是同一产品缺陷，本 lane 只用该用例保留
 * 触发占位，不重复主张；active 行为用例改由 LeakyCaster 绕过析构（见
 * r154_test_util.h），断言未放宽。
 */
TEST_F(R154UdpCasterTest, DISABLED_DestructorAfterActiveStartCrashes)
{
	WTSVariant* cfg = loadCfg("destruct_crash.json",
		R"JSON({"active": true, "sport": 0})JSON");
	ASSERT_NE(cfg, nullptr);

	{
		UDPCaster caster;
		ASSERT_TRUE(caster.init(cfg, nullptr, nullptr));
		cfg->release();
		caster.stop();
	}	// ~UDPCaster：m_ioservice 已析构，m_sktSubscribe 析构时 SIGSEGV
}

/*
 * 触发条件（禁用原因）：UDPCaster::stop（UDPCaster.cpp:137-138）对 m_thrdIO 无条件
 * join；start 之后连续调用两次 stop()，第二次对已 join 的 std::thread 再 join，
 * 抛 std::system_error。与 BUG-R145-001（StateMonitor）同形，但对象/行段不同。
 * 缺陷登记：docs/迭代计划/迭代01/bugs/R154.md BUG-R154-003。
 */
TEST_F(R154UdpCasterTest, DISABLED_SecondStopAfterStartThrows)
{
	WTSVariant* cfg = loadCfg("double_stop.json", R"JSON({
		"active": true, "sport": 0, "bport": 0
	})JSON");
	ASSERT_NE(cfg, nullptr);

	UDPCaster caster;
	ASSERT_TRUE(caster.init(cfg, nullptr, nullptr));
	cfg->release();

	caster.stop();
	caster.stop();	// std::system_error: Invalid argument
}

}	// namespace
