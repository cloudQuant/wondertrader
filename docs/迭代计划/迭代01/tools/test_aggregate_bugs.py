import tempfile
import unittest
from pathlib import Path

import aggregate_bugs


class AggregateBugsParserTests(unittest.TestCase):
    def parse_text(self, text):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "lane.md"
            path.write_text(text, encoding="utf-8")
            return aggregate_bugs.parse_file(path)

    def test_hash_one_bug_headings_split_entries_after_plain_heading(self):
        rows, skipped = self.parse_text(
            """# Lane overview
Context only; this heading is not a defect.

# BUG-R23-001 First defect
- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/first.cpp
- 实际行为: first observed result

# BUG-R23-002 Second defect
- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/second.cpp
- 实际行为: second observed result
"""
        )

        self.assertEqual(["BUG-R23-001", "BUG-R23-002"], [row["id"] for row in rows])
        self.assertEqual(["Major", "Minor"], [row["severity"] for row in rows])
        self.assertEqual(["src/first.cpp", "src/second.cpp"], [row["where"] for row in rows])
        self.assertEqual([], skipped)

    def test_existing_hash_two_headings_and_field_aliases_still_parse(self):
        rows, skipped = self.parse_text(
            """## BUG-R52-001 First existing entry
- 状态: 待审核
- 分级: Blocker
- 位置: src/reader.cpp
- 现象: first result

## BUG-R52-002 Second existing entry
- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/writer.cpp
- 实际行为: second result
"""
        )

        self.assertEqual(["BUG-R52-001", "BUG-R52-002"], [row["id"] for row in rows])
        self.assertEqual(["Blocker", "Major"], [row["severity"] for row in rows])
        self.assertEqual(["src/reader.cpp", "src/writer.cpp"], [row["where"] for row in rows])
        self.assertEqual(["first result", "second result"], [row["actual"] for row in rows])
        self.assertEqual([], skipped)

    def test_only_explicit_terminal_statuses_are_excluded_from_pending_rows(self):
        rows, skipped = self.parse_text(
            """# Lane overview（状态：已合并）
Context only; this is not a defect heading.

# BUG-R23-003 Retracted in heading（状态：已撤回）
- 严重级别: Major
- 模块/文件: src/retracted.cpp

## BUG-R23-004 Merged in heading（已合并）
- 严重级别: Major
- 模块/文件: src/merged-heading.cpp

## BUG-R23-005 Merged in status field
- 状态: 已合并
- 严重级别: Minor
- 模块/文件: src/merged-field.cpp

## BUG-R23-006 Retracted in status field
- 状态: 已撤回（不进入评审清单）
- 严重级别: Minor
- 模块/文件: src/retracted-field.cpp

## BUG-R23-007 Other explicit status remains included
- 状态: 待验证
- 严重级别: Minor
- 模块/文件: src/other-status.cpp

## BUG-R23-008 Missing status remains compatible
- 严重级别: Major
- 模块/文件: src/no-status.cpp

## Notes（状态：已撤回）
This non-candidate heading must not be reported as an unparsed defect.
"""
        )

        self.assertEqual(["BUG-R23-007", "BUG-R23-008"], [row["id"] for row in rows])
        self.assertEqual(["待验证", "待审核"], [row["status"] for row in rows])
        self.assertEqual([], skipped)

    def test_unnumbered_title_entries_include_nonterminal_statuses(self):
        rows, skipped = self.parse_text(
            """# Lane overview
- 标题: Confirmed source issue
  - 状态: 源码缺陷已确认
  - 位置: src/confirmed.cpp

- 标题: Rechecked candidate
  - 状态: 原缺陷论据不成立；边界用例仍可补充
  - 位置: src/rechecked.cpp

- 标题: Retracted entry
  - 状态: 已撤回（不再汇总）
  - 位置: src/retracted.cpp

- 标题: Merged entry
  - 状态: 已合并
  - 位置: src/merged.cpp
"""
        )

        self.assertEqual(2, len(rows))
        self.assertEqual(["Confirmed source issue", "Rechecked candidate"],
                         [row["title"] for row in rows])
        self.assertEqual(["源码缺陷已确认", "原缺陷论据不成立；边界用例仍可补充"],
                         [row["status"] for row in rows])
        self.assertEqual(["src/confirmed.cpp", "src/rechecked.cpp"],
                         [row["where"] for row in rows])
        self.assertEqual([], skipped)

    def test_scalar_fields_ignore_following_nested_bullets(self):
        rows, skipped = self.parse_text(
            """## BUG-R23-009 Retracted with a nested explanation
- 严重级别: Major
  - 子项涉及另一个模块和路径 src/other.cpp
- 状态: 已撤回
  - 撤回原因：测试前置条件不成立
- 输入/前置条件: First condition
  - Additional condition detail
- 实际行为: First observation
  - Additional observed detail
"""
        )

        # Child bullets after scalar fields must not corrupt severity or make a
        # retracted entry reappear in the pending list. Multiline prose fields
        # should still preserve their continuation text.
        self.assertEqual([], rows)
        self.assertEqual([], skipped)

    def test_scalar_fields_do_not_absorb_nested_bullets_on_pending_entry(self):
        rows, skipped = self.parse_text(
            """## BUG-R23-010 Pending source issue
- 严重级别: Major
  - 子项说明不属于级别名称
- 状态: 待验证
  - 备注不属于状态名称
- 输入/前置条件: First condition
  - Additional condition detail
"""
        )

        self.assertEqual(1, len(rows))
        self.assertEqual("Major", rows[0]["severity"])
        self.assertEqual("待验证", rows[0]["status"])
        self.assertEqual("First condition / Additional condition detail", rows[0]["input"])
        self.assertEqual([], skipped)

    def test_explicit_non_finding_review_section_is_not_a_candidate(self):
        rows, skipped = self.parse_text(
            """## BUG-R23-011 Existing defect before review matrix
- 状态: 待审核
- 严重级别: Major
- 位置: src/before.cpp

## 复核记录（非新增缺陷，不重复立案）

| 既有编号 | 本轮结论 |
|---|---|
| BUG-R23-011 | 已复核，未新增缺陷 |

- 状态: 待审核

- 标题: Unnumbered finding after review matrix
  - 状态: 源码缺陷待验证
  - 位置: src/unnumbered.cpp

## BUG-R23-012 Ordinary pending defect after review matrix
- 状态: 待审核
- 严重级别: Minor
- 位置: src/after.cpp
"""
        )

        self.assertEqual(
            ["BUG-R23-011", "lane#2", "BUG-R23-012"],
            [row["id"] for row in rows],
        )
        self.assertEqual("Unnumbered finding after review matrix", rows[1]["title"])
        self.assertEqual("Major", rows[0]["severity"])
        self.assertNotIn("severity", rows[1])
        self.assertEqual("Minor", rows[2]["severity"])
        self.assertEqual("源码缺陷待验证", rows[1]["status"])
        self.assertEqual([], skipped)

    def test_numeric_finding_head_requires_finding_field_signature(self):
        rows, skipped = self.parse_text(
            """## 1. Lane scope
This is an overview, not a finding.

## 2. Cache lookup uses the wrong map
- 位置: src/reader.cpp:100
- 影响: Existing data can be skipped after a queue cache hit.
- 建议: Query the map that owns this cache.

## 3. 交叉引用（不重复立案）
BUG-R74-001 is the canonical record for the shared behavior.

## 4. Merged historical item（状态：已合并）
- 位置: src/old.cpp:10
- 现象: Historical duplicate observation.
- 建议: Use the canonical record.

## 5. Incomplete numeric candidate
- 位置: src/uncertain.cpp:20
"""
        )

        self.assertEqual(["lane#2"], [row["id"] for row in rows])
        self.assertEqual("src/reader.cpp:100", rows[0]["where"])
        self.assertEqual("Existing data can be skipped after a queue cache hit.", rows[0]["impact"])
        self.assertEqual(1, len(skipped))
        self.assertIn("lane#5", skipped[0])
        self.assertNotIn("交叉引用", skipped[0])

    def test_numeric_finding_ids_follow_source_numbers_not_candidate_order(self):
        first, first_skipped = self.parse_text(
            """## 2. First finding
- 位置: src/first.cpp
- 现象: First behavior.

## 8. Second finding
- 位置: src/second.cpp
- 建议: Repair the second behavior.
"""
        )
        with_interstitial_overview, second_skipped = self.parse_text(
            """## 1. Overview
No finding fields here.

## 2. First finding
- 位置: src/first.cpp
- 现象: First behavior.

## 3. 交叉引用（不重复立案）
No defect fields here.

## 8. Second finding
- 位置: src/second.cpp
- 建议: Repair the second behavior.
"""
        )

        self.assertEqual(["lane#2", "lane#8"], [row["id"] for row in first])
        self.assertEqual([row["id"] for row in first], [row["id"] for row in with_interstitial_overview])
        self.assertEqual([], first_skipped)
        self.assertEqual([], second_skipped)

    def test_numeric_id_wins_over_cross_reference_and_legacy_fields_are_signatures(self):
        rows, skipped = self.parse_text(
            """## 1. Uninitialized field (duplicate of BUG-R12-06)
- 位置: src/data.cpp:20
- 影响: Reading this field before initialization is undefined behavior.

## 2. Invalid lookup for a malformed input (see BUG-R18-005)
- 位置: src/manager.cpp:40
- 触发: Pass a single-segment PID to the public lookup API.
"""
        )

        self.assertEqual(["lane#1", "lane#2"], [row["id"] for row in rows])
        self.assertEqual("Reading this field before initialization is undefined behavior.",
                         rows[0]["impact"])
        self.assertEqual("Pass a single-segment PID to the public lookup API.",
                         rows[1]["input"])
        self.assertEqual([], skipped)

    def test_lane_bug_numbering_enters_and_excludes_by_explicit_status(self):
        rows, skipped = self.parse_text(
            """## R21-BUG-04 Pending follow-up
- 状态: 待验证
- 位置: src/reader.cpp
- 实际行为: The cached range is stale.

## R21-BUG-03 Merged duplicate
- 状态: 已合并
- 位置: src/reader.cpp
- 实际行为: Same root cause as the canonical item.

## BUG-P02-003 Existing leading BUG prefix
- 状态: 待审核
- 位置: src/engine.cpp
- 实际行为: The data manager result is discarded.

## R23-005 Existing lane prefix
- 状态: 待审核
- 位置: src/writer.cpp
- 实际行为: The date boundary is inconsistent.

## R4-BUG-07 Another lane-first identifier
- 状态: 待验证
- 位置: src/replay.cpp
- 实际行为: A replay range omits its final item.

## Ordinary notes
This heading is unrelated to a defect entry.
"""
        )

        self.assertEqual(
            ["R21-BUG-04", "BUG-P02-003", "R23-005", "R4-BUG-07"],
            [row["id"] for row in rows],
        )
        self.assertEqual(["待验证", "待审核", "待审核", "待验证"],
                         [row["status"] for row in rows])
        self.assertEqual([], skipped)


if __name__ == "__main__":
    unittest.main()
