/*!
 * \file r164_udp_caster_receive_test.cpp
 * \brief UDPCaster::do_receive 的订阅解析与错误回包（迭代01 第二十四波 R164）
 *
 * 覆盖 UDPCaster.cpp:145-220：
 *  - 非 1024 字节报文 -> L203-216 回 "Can not indentify the command"；
 *  - 1024 字节但 _type != 0x100 -> L162 条件为 false，仅重新挂起接收；
 *  - 1024 字节订阅包 -> StrUtil::split、点号切分 code/exchg、getContract、
 *    getCurTick；DataManager 没有 writer，getCurTick 恒为 NULL，走
 *    L180-182 的 continue（tick 非空时的回包段没有公开接缝，见清单）。
 *
 * 只使用 127.0.0.1 回环：客户端向 UDPCaster 的临时订阅端口发包；
 * 每个“无回包”的请求后追加一个短包作为屏障，收到错误回包即证明前序请求已处理。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <WtDtCore/DataManager.h>
#include <WTSTools/WTSBaseDataMgr.h>

#include "r164_test_util.h"

namespace {

class R164UdpCasterReceiveTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r164::make_tmp_dir("udp_recv");
		r164::write_file(_dir / "sessions.json", R"JSON({
			"DAY": {"name": "日盘", "offset": 0, "sections": [{"from": 930, "to": 1500}]}
		})JSON");
		r164::write_file(_dir / "commodities.json", R"JSON({
			"SHFE": {"ag": {"name": "白银", "session": "DAY", "holiday": "R164_NONE"}}
		})JSON");
		r164::write_file(_dir / "contracts.json", R"JSON({
			"SHFE": {"ag2512": {"name": "ag2512", "exchg": "SHFE", "product": "ag"}}
		})JSON");

		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadContracts((_dir / "contracts.json").string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	std::filesystem::path _dir;
	WTSBaseDataMgr _bdMgr;
	DataManager _dtMgr;
};

/*
 * UDPCaster.cpp:145-220
 * 三段请求依次驱动回调：
 *  1) 短包 -> 错误回包（覆盖回包 lambda 的 delete 与 ec==false 路径）；
 *  2) 1024 字节未知 type -> 不匹配 L162，无回包；屏障短包确认已处理；
 *  3) 订阅包 "ag2512,SHFE.ag2512,NO.NOPE"：
 *     - "ag2512" 无点号 -> exchg 为空，getContract 返回 NULL -> continue（L177-178）；
 *     - "SHFE.ag2512" -> 合约存在，getCurTick 因无 writer 返回 NULL -> continue（L180-182）；
 *     - "NO.NOPE" -> 合约不存在 -> continue。
 *     订阅分支不回包；屏障短包的错误回包证明整包已按序处理完。
 * stop() 只 join io 线程（本用例不触发 broadcast，m_thrdCast 为空）。
 */
TEST_F(R164UdpCasterReceiveTest, MalformedAndSubscribeDatagramsOnLoopback)
{
	const int sport = r164::pick_free_udp_port();
	ASSERT_NE(sport, 0);

	WTSVariant* cfg = r164::load_cfg(_dir, "receive.json",
		std::string(R"JSON({"active": true, "sport": )JSON")
		+ std::to_string(sport) + R"JSON(})JSON");
	ASSERT_NE(cfg, nullptr);

	r164::LeakyCaster caster;
	ASSERT_TRUE(caster->init(cfg, &_bdMgr, &_dtMgr));
	cfg->release();
	caster.arm();

	const int clientFd = r164::bind_udp_loopback(nullptr);
	ASSERT_GE(clientFd, 0);

	std::vector<char> reply;
	const uint32_t junk = 0x12345678u;

	// 1) 短包：长度不等于 1024，直接回错误文本。
	ASSERT_TRUE(r164::send_datagram(clientFd, static_cast<uint16_t>(sport),
		&junk, sizeof(junk)));
	ASSERT_GT(r164::recv_datagram(clientFd, reply, 3000), 0);
	EXPECT_EQ(std::string(reply.begin(), reply.end()), r164::cannot_identify_text());

	// 2) 1024 字节但 type=0x999：不进入订阅分支，也不回包。
	r164::ReqPacket unknown;
	memset(&unknown, 0, sizeof(unknown));
	unknown._type = 0x999u;
	ASSERT_TRUE(r164::send_datagram(clientFd, static_cast<uint16_t>(sport),
		&unknown, sizeof(unknown)));
	ASSERT_TRUE(r164::send_datagram(clientFd, static_cast<uint16_t>(sport),
		&junk, sizeof(junk)));
	ASSERT_GT(r164::recv_datagram(clientFd, reply, 3000), 0);
	EXPECT_EQ(std::string(reply.begin(), reply.end()), r164::cannot_identify_text());

	// 3) 订阅包：三种代码分别命中空 exchg、有效合约 + NULL tick、无效合约。
	r164::ReqPacket sub;
	memset(&sub, 0, sizeof(sub));
	sub._type = 0x100u;
	const char* codes = "ag2512,SHFE.ag2512,NO.NOPE";
	memcpy(sub._data, codes, strlen(codes));
	ASSERT_TRUE(r164::send_datagram(clientFd, static_cast<uint16_t>(sport),
		&sub, sizeof(sub)));
	ASSERT_TRUE(r164::send_datagram(clientFd, static_cast<uint16_t>(sport),
		&junk, sizeof(junk)));
	ASSERT_GT(r164::recv_datagram(clientFd, reply, 3000), 0);
	EXPECT_EQ(std::string(reply.begin(), reply.end()), r164::cannot_identify_text());

	caster.stop_once();
	::close(clientFd);
}

}	// namespace
