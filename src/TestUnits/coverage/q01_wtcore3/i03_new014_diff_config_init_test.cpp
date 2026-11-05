/**
 * i03_new014_diff_config_init_test.cpp —— I03-NEW-014（迭代03，2026-11-04，exec-units 包复验）
 *
 * WtDiffExecuter::_config 在构造时必须为 NULL：未 init 或 init 失败的宿主收到目标后，
 * getUnit() 依赖 `_config == NULL` 在进入任何工厂/插件代码前安全失败。若该成员未初始化，
 * 堆上对象会读到分配器填充值（ASan 0xbe、macOS MallocPreScribble 0xaa）并解引用。
 * 运行时建议配合 MallocPreScribble=1（或 ASan）以使未初始化读取可观察。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#include <Includes/WTSVariant.hpp>
#include <WtCore/ExecUnitInitStatus.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtExecuterFactory.h>
#include <WtCore/WtHelper.h>

using namespace wtp;

TEST(I03New014DiffConfig, UninitializedHostRejectsUnitCreationBeforeReadingConfig)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("i03_new014_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    WtHelper::setGenerateDir((dir.string() + "/").c_str());

    WtExecuterFactory factory;
    for (int round = 0; round < 8; ++round)
    {
        // Heap objects make an uninitialized member observable under allocator scribbling.
        auto diff = std::make_shared<WtDiffExecuter>(&factory,
            ("i03_new014_" + std::to_string(round)).c_str(), nullptr, nullptr);
        EXPECT_FALSE(diff->init(nullptr));
        WTSVariant* bad = WTSVariant::createObject(); // missing scale/policy: rejected eagerly
        EXPECT_FALSE(diff->init(bad));
        bad->release();

        diff->on_position_changed("SHFE.ag.2512", 1.0);
        const ExecUnitInitStatus status = diff->queryUnitInitStatus("SHFE.ag.2512");
        EXPECT_EQ(status.state, ExecUnitInitState::FAILED);
        EXPECT_NE(status.error.find("executor configuration is missing"), std::string::npos)
            << status.error;
    }
    std::filesystem::remove_all(dir, ec);
}
