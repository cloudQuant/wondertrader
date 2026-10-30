/*!
 * \file r16_lmdb_test.cpp
 * \brief R16 覆盖率用例：src/WTSUtils/WtLMDB.hpp（WtLMDB + WtLMDBQuery）
 *
 * 只使用本地临时目录里的 LMDB 环境（普通文件 + 文件映射），不起线程、不读挂钟、不碰 socket。
 * 每个用例一个独立目录（pid + tag 隔离），可以并行跑。
 */
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
#include "WtLMDB.hpp"
#include "r16_tmp.h"

namespace wtp {

struct WtLMDBTestAccess
{
	static bool open_with_mapsize_error(WtLMDB& db, const char* path, std::size_t mapsize)
	{
		return db.open_impl(path, mapsize, &fail_mapsize);
	}

private:
	static int fail_mapsize(MDB_env*, std::size_t)
	{
		return MDB_MAP_FULL;
	}
};

} // namespace wtp

namespace {

// 在写入事务里塞进 n 个单字符键 a..（n<=26），值与键一一对应
void seed_keys(wtp::WtLMDB& db, char first, int n)
{
	std::vector<std::string> keys;
	for (int i = 0; i < n; i++)
		keys.emplace_back(1, (char)(first + i));

	wtp::WtLMDBQuery q(db);
	for (size_t i = 0; i < keys.size(); i++)
		ASSERT_TRUE(q.put(keys[i], std::string("v") + keys[i]));
	q.commit();
}
struct Captured
{
	std::vector<std::string> keys;
	std::vector<std::string> vals;
	int calls{0};
};

wtp::LMDBQueryCallback capture(Captured* out)
{
	return [out](const wtp::ValueArray& ks, const wtp::ValueArray& vs) {
		out->calls++;
		for (size_t i = 0; i < ks.size(); i++) out->keys.emplace_back(ks[i]);
		for (size_t i = 0; i < vs.size(); i++) out->vals.emplace_back(vs[i]);
	};
}

}	// namespace

TEST(r16_lmdb, open_fresh_env_sets_handle_and_no_error)
{
	const std::string dir = r16tmp::file("lmdb1", "env");
	r16tmp::clean("lmdb1");
	{
		wtp::WtLMDB db;
		EXPECT_EQ(nullptr, (void*)db.env());
		EXPECT_EQ(0u, (unsigned int)db.dbi());
		EXPECT_FALSE(db.has_error());
		EXPECT_FALSE(db.is_readonly());
		ASSERT_TRUE(db.open(dir.c_str()));
		EXPECT_NE(nullptr, (void*)db.env());
	}
	r16tmp::clean("lmdb1");
}

TEST(r16_lmdb, put_then_get_range_returns_exact_window)
{
	const std::string dir = r16tmp::file("lmdb2", "env");
	r16tmp::clean("lmdb2");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		seed_keys(db, 'a', 4);		// a,b,c,d

		Captured cap;
		wtp::WtLMDBQuery q(db);
		int cnt = q.get_range("b", "c", capture(&cap));
		EXPECT_EQ(2, cnt);
		EXPECT_EQ(1, cap.calls);
		ASSERT_EQ((size_t)2, cap.keys.size());
		ASSERT_EQ((size_t)2, cap.vals.size());
		EXPECT_EQ(std::string("b"), cap.keys[0]);
		EXPECT_EQ(std::string("c"), cap.keys[1]);
		EXPECT_EQ(std::string("vb"), cap.vals[0]);
		EXPECT_EQ(std::string("vc"), cap.vals[1]);
		EXPECT_FALSE(db.has_error());
	}
	r16tmp::clean("lmdb2");
}

TEST(r16_lmdb, get_range_compares_variable_length_binary_keys)
{
	EXPECT_LT(wtp::compare_lmdb_keys(nullptr, 0, "a", 1), 0);
	EXPECT_GT(wtp::compare_lmdb_keys("a", 1, nullptr, 0), 0);
	EXPECT_EQ(0, wtp::compare_lmdb_keys(nullptr, 0, nullptr, 0));

	const std::string dir = r16tmp::file("lmdb14", "env");
	r16tmp::clean("lmdb14");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		const std::string embedded_nul("ab\0", 3);
		const std::string embedded_nul_longer("ab\0x", 4);
		{
			wtp::WtLMDBQuery q(db);
			ASSERT_TRUE(q.put(std::string("a"), std::string("short")));
			ASSERT_TRUE(q.put(std::string("ab"), std::string("exact")));
			ASSERT_TRUE(q.put(embedded_nul, std::string("nul")));
			ASSERT_TRUE(q.put(embedded_nul_longer, std::string("longer")));
			ASSERT_TRUE(q.put(std::string("abx"), std::string("suffix")));
			q.commit();
		}

		Captured cap;
		wtp::WtLMDBQuery q(db);
		EXPECT_EQ(2, q.get_range("a", "ab", capture(&cap)));
		ASSERT_EQ((size_t)2, cap.keys.size());
		EXPECT_EQ(std::string("a"), cap.keys[0]);
		EXPECT_EQ(std::string("ab"), cap.keys[1]);
		EXPECT_EQ(std::string("exact"), cap.vals[1]);

		Captured empty_lower_cap;
		EXPECT_EQ(1, q.get_range(std::string(), std::string("a"), capture(&empty_lower_cap)));
		ASSERT_EQ((size_t)1, empty_lower_cap.keys.size());
		EXPECT_EQ(std::string("a"), empty_lower_cap.keys[0]);

		Captured empty_cap;
		EXPECT_EQ(0, q.get_range(std::string(), std::string(), capture(&empty_cap)));
		EXPECT_EQ(1, empty_cap.calls);
		EXPECT_TRUE(empty_cap.keys.empty());
		EXPECT_TRUE(empty_cap.vals.empty());

		Captured binary_cap;
		EXPECT_EQ(2, q.get_range(std::string("ab"), embedded_nul, capture(&binary_cap)));
		ASSERT_EQ((size_t)2, binary_cap.keys.size());
		EXPECT_EQ(std::string("ab"), binary_cap.keys[0]);
		EXPECT_EQ(embedded_nul, binary_cap.keys[1]);

		const std::string between_binary_keys("ab\0w", 4);
		Captured binary_window;
		EXPECT_EQ(2, q.get_range(std::string("ab"), between_binary_keys, capture(&binary_window)));
		ASSERT_EQ((size_t)2, binary_window.keys.size());
		EXPECT_EQ(std::string("ab"), binary_window.keys[0]);
		EXPECT_EQ(embedded_nul, binary_window.keys[1]);

		Captured upper_cap;
		EXPECT_EQ(2, q.get_uppers("a", "ab", 10, capture(&upper_cap)));
		ASSERT_EQ((size_t)2, upper_cap.keys.size());
		EXPECT_EQ(std::string("a"), upper_cap.keys[0]);
		EXPECT_EQ(std::string("ab"), upper_cap.keys[1]);

		Captured upper_binary_cap;
		EXPECT_EQ(2, q.get_uppers(std::string("ab"), between_binary_keys, 10, capture(&upper_binary_cap)));
		ASSERT_EQ((size_t)2, upper_binary_cap.keys.size());
		EXPECT_EQ(std::string("ab"), upper_binary_cap.keys[0]);
		EXPECT_EQ(embedded_nul, upper_binary_cap.keys[1]);

		Captured lower_cap;
		EXPECT_EQ(2, q.get_lowers("a", "ab", 10, capture(&lower_cap)));
		ASSERT_EQ((size_t)2, lower_cap.keys.size());
		EXPECT_EQ(std::string("a"), lower_cap.keys[0]);
		EXPECT_EQ(std::string("ab"), lower_cap.keys[1]);

		Captured lower_binary_cap;
		EXPECT_EQ(2, q.get_lowers(std::string("ab"), between_binary_keys, 10, capture(&lower_binary_cap)));
		ASSERT_EQ((size_t)2, lower_binary_cap.keys.size());
		EXPECT_EQ(std::string("ab"), lower_binary_cap.keys[0]);
		EXPECT_EQ(embedded_nul, lower_binary_cap.keys[1]);
	}
	r16tmp::clean("lmdb14");
}

TEST(r16_lmdb, get_uppers_walks_forward_until_count_or_upper)
{
	const std::string dir = r16tmp::file("lmdb3", "env");
	r16tmp::clean("lmdb3");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		seed_keys(db, 'a', 4);		// a,b,c,d

		{
			Captured cap;
			wtp::WtLMDBQuery q(db);
			// 不设数量上限时，count 给一个足够大的值：命中 b,c,d 后越过 d 的下一个才停
			int cnt = q.get_uppers("b", "d", 100, capture(&cap));
			EXPECT_EQ(3, cnt);
			ASSERT_EQ((size_t)3, cap.keys.size());
			EXPECT_EQ(std::string("b"), cap.keys[0]);
			EXPECT_EQ(std::string("d"), cap.keys[2]);
		}
		{
			Captured cap;
			wtp::WtLMDBQuery q(db);
			int cnt = q.get_uppers("a", "d", 2, capture(&cap));
			EXPECT_EQ(2, cnt);
			ASSERT_EQ((size_t)2, cap.keys.size());
			EXPECT_EQ(std::string("a"), cap.keys[0]);
			EXPECT_EQ(std::string("b"), cap.keys[1]);
		}
	}
	r16tmp::clean("lmdb3");
}

TEST(r16_lmdb, get_lowers_walks_backward_then_reverses)
{
	const std::string dir = r16tmp::file("lmdb4", "env");
	r16tmp::clean("lmdb4");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		seed_keys(db, 'a', 4);		// a,b,c,d

		{
			Captured cap;
			wtp::WtLMDBQuery q(db);
			// 从 c 往回找 2 条：拿到 c、b，reverse 之后是 b,c
			int cnt = q.get_lowers("b", "c", 2, capture(&cap));
			EXPECT_EQ(2, cnt);
			ASSERT_EQ((size_t)2, cap.keys.size());
			EXPECT_EQ(std::string("b"), cap.keys[0]);
			EXPECT_EQ(std::string("c"), cap.keys[1]);
			EXPECT_EQ(std::string("vb"), cap.vals[0]);
		}
		{
			Captured cap;
			wtp::WtLMDBQuery q(db);
			// 上边界 'z' 不在库里 → SET_RANGE 未命中 → 走 MDB_LAST 兜底分支
			int cnt = q.get_lowers("c", "z", 5, capture(&cap));
			EXPECT_EQ(2, cnt);
			ASSERT_EQ((size_t)2, cap.keys.size());
			EXPECT_EQ(std::string("c"), cap.keys[0]);
			EXPECT_EQ(std::string("d"), cap.keys[1]);
		}
	}
	r16tmp::clean("lmdb4");
}

TEST(r16_lmdb, put_and_commit_variant_persists_without_destructor_commit)
{
	const std::string dir = r16tmp::file("lmdb5", "env");
	r16tmp::clean("lmdb5");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		{
			wtp::WtLMDBQuery q(db);
			EXPECT_TRUE(q.put_and_commit("pk", "pv"));
			// put_and_commit 已经提交并把 _commited 置位，析构不再重复提交
		}
		Captured cap;
		wtp::WtLMDBQuery q(db);
		EXPECT_EQ(1, q.get_range("pk", "pk", capture(&cap)));
		ASSERT_EQ((size_t)1, cap.vals.size());
		EXPECT_EQ(std::string("pv"), cap.vals[0]);
	}
	r16tmp::clean("lmdb5");
}

TEST(r16_lmdb, rollback_drops_uncommitted_writes)
{
	const std::string dir = r16tmp::file("lmdb6", "env");
	r16tmp::clean("lmdb6");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		seed_keys(db, 'a', 1);		// 只有 a

		{
			wtp::WtLMDBQuery q(db);
			ASSERT_TRUE(q.put("ghost", "boo"));
			q.rollback();
			q.rollback();			// 已提交/已回滚时直接 return，不二次 abort
		}
		Captured cap;
		wtp::WtLMDBQuery q(db);
		EXPECT_EQ(1, q.get_range("a", "z", capture(&cap)));
		EXPECT_EQ(std::string("a"), cap.keys[0]);
	}
	r16tmp::clean("lmdb6");
}

TEST(r16_lmdb, commit_is_idempotent_and_reuses_dbi)
{
	const std::string dir = r16tmp::file("lmdb7", "env");
	r16tmp::clean("lmdb7");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		MDB_dbi first = 0;
		{
			wtp::WtLMDBQuery q(db);
			first = db.dbi();
			EXPECT_NE(0u, (unsigned int)first);	// update_dbi 在构造里开了 dbi
			ASSERT_TRUE(q.put("k", "v"));
			q.commit();
			q.commit();			// _commited 已置位 → 直接 return
		}
		{
			// 第二个事务复用同一个 _dbi（update_dbi 的短路分支）
			wtp::WtLMDBQuery q(db);
			EXPECT_EQ(first, db.dbi());
			Captured cap;
			EXPECT_EQ(1, q.get_range("k", "k", capture(&cap)));
		}
	}
	r16tmp::clean("lmdb7");
}

TEST(r16_lmdb, error_accessors_reflect_manual_errno_update)
{
	const std::string dir = r16tmp::file("lmdb8", "env");
	r16tmp::clean("lmdb8");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		EXPECT_FALSE(db.has_error());
		db.update_errno(MDB_NOTFOUND);
		EXPECT_TRUE(db.has_error());
		const char* msg = db.errmsg();
		ASSERT_NE(nullptr, msg);
		EXPECT_LT((size_t)0, strlen(msg));
		db.update_errno(MDB_SUCCESS);
		EXPECT_FALSE(db.has_error());
	}
	r16tmp::clean("lmdb8");
}

TEST(r16_lmdb, readonly_env_reports_readonly_and_rejects_put)
{
	const std::string dir = r16tmp::file("lmdb9", "env");
	r16tmp::clean("lmdb9");
	{
		{
			wtp::WtLMDB db;
			ASSERT_TRUE(db.open(dir.c_str()));
			seed_keys(db, 'a', 1);
		}
		// 前一个 env 必须先关掉，再在同一目录上开只读 env（同进程双 env 是 LMDB 不推荐的用法）
		{
			wtp::WtLMDB ro(true);
			ASSERT_TRUE(ro.open(dir.c_str()));
			EXPECT_TRUE(ro.is_readonly());
			wtp::WtLMDBQuery q(ro);
			// 只读事务写入 → mdb_put 失败 → put 返回 false（WtLMDB.hpp 的失败分支）
			EXPECT_FALSE(q.put("new", "val"));
			EXPECT_TRUE(ro.has_error());
		}
	}
	r16tmp::clean("lmdb9");
}

TEST(r16_lmdb, get_looks_up_the_requested_key)
{
	// WtLMDBQuery::get(key) 的接口是按传入键读取；缺失键应返回空串。
	const std::string dir = r16tmp::file("lmdb10", "env");
	r16tmp::clean("lmdb10");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		seed_keys(db, 'a', 2);
		wtp::WtLMDBQuery q(db);
		EXPECT_EQ(std::string("va"), q.get(std::string("a")));
		EXPECT_EQ(std::string("vb"), q.get(std::string("b")));
		EXPECT_EQ(std::string(""), q.get(std::string("does_not_exist")));
		const std::string binaryKey("a\0x", 3);
		const std::string binaryValue("v\0x", 3);
		ASSERT_TRUE(q.put(binaryKey, binaryValue));
		EXPECT_EQ(binaryValue, q.get(binaryKey));
	}
	r16tmp::clean("lmdb10");
}

TEST(r16_lmdb, get_all_returns_each_record_once)
{
	// get_all 应将每个 key/value 恰好回调一次，返回数应与库中的记录数一致。
	const std::string dir = r16tmp::file("lmdb11", "env");
	r16tmp::clean("lmdb11");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		seed_keys(db, 'x', 2);		// x, y

		Captured cap;
		wtp::WtLMDBQuery q(db);
		EXPECT_THROW(q.get_all([](const wtp::ValueArray&, const wtp::ValueArray&) {
			throw std::runtime_error("callback failure");
		}), std::runtime_error);
		int cnt = q.get_all(capture(&cap));
		EXPECT_EQ(2, cnt);
		EXPECT_EQ((size_t)2, cap.keys.size());
		EXPECT_EQ((size_t)2, cap.vals.size());
		if (cap.keys.size() == 2 && cap.vals.size() == 2)
		{
			EXPECT_EQ(std::string("x"), cap.keys[0]);
			EXPECT_EQ(std::string("y"), cap.keys[1]);
			EXPECT_EQ(std::string("vx"), cap.vals[0]);
			EXPECT_EQ(std::string("vy"), cap.vals[1]);
		}
		EXPECT_EQ(1, cap.calls);
		EXPECT_FALSE(db.has_error());
	}
	r16tmp::clean("lmdb11");
}

TEST(r16_lmdb, get_all_on_empty_db_returns_no_records)
{
	const std::string dir = r16tmp::file("lmdb12", "env");
	r16tmp::clean("lmdb12");
	{
		wtp::WtLMDB db;
		ASSERT_TRUE(db.open(dir.c_str()));
		Captured cap;
		wtp::WtLMDBQuery q(db);
		EXPECT_EQ(0, q.get_all(capture(&cap)));
		EXPECT_TRUE(cap.keys.empty());
		EXPECT_TRUE(cap.vals.empty());
		EXPECT_EQ(1, cap.calls);
		EXPECT_EQ(0, q.get_all(capture(&cap)));
		EXPECT_EQ(2, cap.calls);
		EXPECT_FALSE(db.has_error());
	}
	r16tmp::clean("lmdb12");
}

TEST(r16_lmdb, open_failure_updates_member_error)
{
	// Failed open must expose its error through the public has_error()/errmsg() accessors.
	const std::string filePath = r16tmp::file("lmdb13", "not_a_dir.txt");
	{
		std::ofstream f(filePath, std::ios::binary);
		f << "x";
		f.close();
	}
	{
		wtp::WtLMDB db;
		EXPECT_FALSE(db.open(filePath.c_str()));		// 同名普通文件不是环境目录
		EXPECT_TRUE(db.has_error());
		EXPECT_EQ(nullptr, (void*)db.env());
		const char* msg = db.errmsg();
		ASSERT_NE(nullptr, msg);
		EXPECT_NE('\0', msg[0]);
	}
	r16tmp::clean("lmdb13");
}

TEST(r16_lmdb, mapsize_failure_reports_error_closes_environment_and_can_retry)
{
	const std::string dir = r16tmp::file("lmdb15", "env");
	r16tmp::clean("lmdb15");
	{
		wtp::WtLMDB db;
		ASSERT_FALSE(wtp::WtLMDBTestAccess::open_with_mapsize_error(db, dir.c_str(), 16 * 1024 * 1024));
		EXPECT_TRUE(db.has_error());
		EXPECT_EQ(nullptr, (void*)db.env());
		const char* message = db.errmsg();
		ASSERT_NE(nullptr, message);
		EXPECT_STREQ(mdb_strerror(MDB_MAP_FULL), message);

		// The failed map-size setup must release the partial environment so the
		// same wrapper can open successfully on a later attempt.
		EXPECT_TRUE(db.open(dir.c_str()));
		EXPECT_FALSE(db.has_error());
		EXPECT_NE(nullptr, (void*)db.env());
	}
	r16tmp::clean("lmdb15");
}
