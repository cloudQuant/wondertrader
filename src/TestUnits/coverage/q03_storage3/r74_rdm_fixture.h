// r74 / WtDataStorage 补测车道自用夹具（非用例 TU）
//
// 只补 r52::HotMgr 的一个缺口：r52 的 getRuleTag 用 `strncmp(stdCode, tag, ...)`，
// 而生产 stdCode 形如 "SHFE.rb.HOT"，前缀永远不是 "HOT" → `_ruletag` 实际一直为空，
// 导致 R52/R70 的“主力连续”用例其实都没有进入 HOT 分支。
// 这里返回「包含 .HOT 即命中」，让 CodeHelper::extractStdCode 真正写入 _ruletag，
// 从而能驱动 WtRdmDtReader/WtDataReader 的 HOT 文件探测与切段装配。
//
// 仍然没有任何访问权限宏；数据路径全部由 q03::Sandbox 的临时目录驱动。
#pragma once

#include "r52_storage_fixture.h"

namespace r74 {

class HotMgr : public q03::HotMgr
{
public:
	void set_enabled(bool v) { _enabled = v; }
	void set_raw(const char* code) { _raw = code == NULL ? "" : code; }
	void add_section(const char* code, uint32_t sdate, uint32_t edate, double factor)
	{
		_secs.emplace_back(code, sdate, edate, factor);
	}
	void clear_sections() { _secs.clear(); }
	void set_split_ret(bool ret) { _split_ret = ret; }
	void set_split_empty(bool empty) { _split_empty = empty; }
	uint32_t split_calls() const { return _split_calls; }

	const char* getRuleTag(const char* stdCode) override
	{
		if (!_enabled || stdCode == NULL)
			return "";
		// CodeHelper::extractStdCode 传进来的是第三段（如 "HOT" / "HOT+"），不是完整 stdCode
		return strstr(stdCode, "HOT") != NULL ? "HOT" : "";
	}

	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return _raw.c_str(); }

	bool splitCustomSections(const char* tag, const char*, uint32_t, uint32_t, HotSections& out) override
	{
		_split_calls++;
		if (!_enabled || tag == NULL || strcmp(tag, "HOT") != 0)
			return false;
		if (!_split_ret)
			return false;
		if (!_split_empty)
			out.insert(out.end(), _secs.begin(), _secs.end());
		return true;
	}

private:
	bool		_enabled = false;
	bool		_split_ret = true;
	bool		_split_empty = false;
	std::string	_raw;
	HotSections	_secs;
	uint32_t	_split_calls = 0;
};

} // namespace r74
