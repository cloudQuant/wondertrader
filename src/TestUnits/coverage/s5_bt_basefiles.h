/*!
 * \file s5_bt_basefiles.h
 * \brief 迭代03 第五会话：回测夹具的合约元数据与费率模板（I03-NEW-053/080，卡13d）。
 *
 * 卡13d 后回放器按“实际成交合约”计费：成交合约必须已在基础数据中登记，且合约级或品种级
 * 费率必须存在（合法零费率有效），否则成交被费用门禁拒绝。发行配置 dist/WtBtRunner/configbt.yaml
 * 本就加载 contracts；旧测试夹具只加载品种、且发行 fees.json 不含股票品种。本头文件为夹具生成：
 *   - 一份只含测试所用合约的 contracts 文件（不含用作“未知代码”的 999999 等）；
 *   - 一份费率模板：发行 dist/common/fees.json 全部条目 + SSE/SZSE 股票品种零费率，
 *     与卡13d 之前“股票缺费即 0”的旧期望一致。
 * 只改测试输入，不放宽产品校验。
 */
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace s5bt
{
	namespace fs = std::filesystem;

	/// 本进程专用的夹具输出目录（系统临时目录下，不写仓库与当前目录）。
	inline fs::path scratch_dir()
	{
		return fs::temp_directory_path() / ("s5_bt_basefiles_" + std::to_string(static_cast<long>(::getpid())));
	}

	/// 回测各 lane 用例实际交易/查询的物理合约（std code）。
	inline const std::vector<std::string>& default_codes()
	{
		static const std::vector<std::string> codes = {
			"CFFEX.IF.2001", "CFFEX.IF.2002", "DCE.a.2105", "DCE.i.2409", "DCE.j.2409", "DCE.m.2405",
			"SHFE.ag.2406", "SHFE.ag.2412", "SHFE.cu.2406", "SHFE.hc.2405", "SHFE.hc.2505",
			"SHFE.rb.2001", "SHFE.rb.2405", "SHFE.ru.2406",
			"SSE.STK.000001", "SSE.STK.000002", "SSE.STK.000003", "SSE.STK.000004", "SSE.STK.000005",
			"SSE.STK.000006", "SSE.STK.000007", "SSE.STK.000008", "SSE.STK.000009", "SSE.STK.000010",
			"SSE.STK.000011", "SSE.STK.600000", "SSE.STK.600001", "SSE.STK.600002", "SSE.STK.600003",
			"SSE.STK.600004", "SSE.STK.600005", "SSE.STK.600010", "SSE.STK.600011", "SSE.STK.600020",
			"SSE.STK.600021", "SSE.STK.600022", "SSE.STK.600023", "SSE.STK.600024",
			"SZSE.STK.000001", "SZSE.STK.000002"
		};
		return codes;
	}

	/// "SHFE.rb.2405" -> exchange SHFE, product rb, raw code rb2405；"SSE.STK.600000" -> 600000。
	inline bool split_code(const std::string& stdCode, std::string& exchg, std::string& product, std::string& raw)
	{
		const size_t p1 = stdCode.find('.');
		const size_t p2 = p1 == std::string::npos ? std::string::npos : stdCode.find('.', p1 + 1);
		if (p2 == std::string::npos)
			return false;
		exchg = stdCode.substr(0, p1);
		product = stdCode.substr(p1 + 1, p2 - p1 - 1);
		const std::string tail = stdCode.substr(p2 + 1);
		raw = (product == "STK") ? tail : product + tail;
		return true;
	}

	/// 生成只含给定合约的 contracts 文件，返回路径。
	inline std::string write_contracts(const fs::path& dir, const std::vector<std::string>& codes = default_codes())
	{
		std::map<std::string, std::vector<std::pair<std::string, std::string>>> byExchg;
		for (const std::string& c : codes)
		{
			std::string ex, pid, raw;
			if (split_code(c, ex, pid, raw))
				byExchg[ex].emplace_back(raw, pid);
		}
		std::ostringstream os;
		os << "{";
		bool firstEx = true;
		for (const auto& ex : byExchg)
		{
			os << (firstEx ? "" : ",") << "\"" << ex.first << "\":{";
			firstEx = false;
			bool first = true;
			for (const auto& item : ex.second)
			{
				os << (first ? "" : ",") << "\"" << item.first << "\":{\"name\":\"" << item.first
					<< "\",\"code\":\"" << item.first << "\",\"exchg\":\"" << ex.first
					<< "\",\"product\":\"" << item.second
					<< "\",\"opendate\":19900101,\"expiredate\":20991231}";
				first = false;
			}
			os << "}";
		}
		os << "}";
		std::error_code ec;
		fs::create_directories(dir, ec);
		const fs::path file = dir / "s5_bt_contracts.json";
		std::ofstream(file, std::ios::binary | std::ios::trunc) << os.str();
		return file.string();
	}

	/// 为给定合约所属品种生成零费率模板（卡13d：合法零费率有效），对应旧夹具“未配置费率即 0”的期望。
	inline std::string write_zero_fees(const fs::path& dir, const std::vector<std::string>& codes = default_codes())
	{
		std::map<std::string, bool> products;
		for (const std::string& c : codes)
		{
			std::string ex, pid, raw;
			if (split_code(c, ex, pid, raw))
				products[ex + "." + pid] = true;
		}
		std::ostringstream os;
		os << "{";
		bool first = true;
		for (const auto& p : products)
		{
			os << (first ? "" : ",") << "\"" << p.first
				<< "\":{\"open\":0,\"close\":0,\"closetoday\":0,\"byvolume\":false}";
			first = false;
		}
		os << "}";
		std::error_code ec;
		fs::create_directories(dir, ec);
		const fs::path file = dir / "s5_bt_zero_fees.json";
		std::ofstream(file, std::ios::binary | std::ios::trunc) << os.str();
		return file.string();
	}

	/// 发行 fees.json + 股票品种零费率，返回路径。
	inline std::string write_fees(const fs::path& root, const fs::path& dir)
	{
		std::ifstream in(root / "dist/common/fees.json", std::ios::binary);
		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF)
			text.erase(0, 3);
		const size_t close = text.rfind('}');
		const std::string stocks =
			"\"SSE.STK\":{\"open\":0,\"close\":0,\"closetoday\":0,\"byvolume\":false},"
			"\"SZSE.STK\":{\"open\":0,\"close\":0,\"closetoday\":0,\"byvolume\":false}";
		if (close != std::string::npos)
			text = text.substr(0, close) + "," + stocks + "}";
		else
			text = "{" + stocks + "}";
		std::error_code ec;
		fs::create_directories(dir, ec);
		const fs::path file = dir / "s5_bt_fees.json";
		std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
		return file.string();
	}
}
