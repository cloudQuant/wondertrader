#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// 自定义监听器类
class FailureOnlyEventListener : public ::testing::TestEventListener {
 public:
  explicit FailureOnlyEventListener(::testing::TestEventListener* default_listener)
      : default_listener_(default_listener), original_cout_(nullptr) {}

  void OnTestProgramStart(const ::testing::UnitTest& unit_test) override {
    default_listener_->OnTestProgramStart(unit_test);
  }

  void OnTestIterationStart(const ::testing::UnitTest& unit_test, int iteration) override {
    default_listener_->OnTestIterationStart(unit_test, iteration);
  }

  void OnEnvironmentsSetUpStart(const ::testing::UnitTest& unit_test) override {
    default_listener_->OnEnvironmentsSetUpStart(unit_test);
  }

  void OnEnvironmentsSetUpEnd(const ::testing::UnitTest& unit_test) override {
    default_listener_->OnEnvironmentsSetUpEnd(unit_test);
  }

  void OnTestStart(const ::testing::TestInfo& test_info) override {
    // 在测试开始时，重定向 std::cout 到一个字符串流
    current_test_output_ = std::make_unique<std::ostringstream>();
    original_cout_ = std::cout.rdbuf(current_test_output_->rdbuf());
    current_test_name_ = std::string(test_info.test_suite_name()) + "." + test_info.name();
    default_listener_->OnTestStart(test_info);
  }

  void OnTestPartResult(const ::testing::TestPartResult& test_part_result) override {
    default_listener_->OnTestPartResult(test_part_result);

    // 记录失败部分的详细信息
    if (test_part_result.failed()) {
      std::ostringstream failure_details;
      failure_details << test_part_result.file_name() << ":" << test_part_result.line_number() << "\n"
                      << test_part_result.summary();
      test_failures_[current_test_name_].push_back(failure_details.str());
    }
  }

  void OnTestEnd(const ::testing::TestInfo& test_info) override {
    // 恢复 std::cout 的原始缓冲区
    std::cout.rdbuf(original_cout_);

    if (!test_info.result()->Passed()) {
      // 保存失败测试的输出内容
      failed_tests_.push_back(current_test_name_);
      test_outputs_[current_test_name_] = current_test_output_->str();
    }

    current_test_output_.reset();
    current_test_name_.clear();
    default_listener_->OnTestEnd(test_info);
  }

  void OnEnvironmentsTearDownStart(const ::testing::UnitTest& unit_test) override {
    default_listener_->OnEnvironmentsTearDownStart(unit_test);
  }

  void OnEnvironmentsTearDownEnd(const ::testing::UnitTest& unit_test) override {
    default_listener_->OnEnvironmentsTearDownEnd(unit_test);
  }

  void OnTestIterationEnd(const ::testing::UnitTest& unit_test, int iteration) override {
    default_listener_->OnTestIterationEnd(unit_test, iteration);
  }

  void OnTestProgramEnd(const ::testing::UnitTest& unit_test) override {
    // 输出失败测试的详细信息
    if (!failed_tests_.empty()) {
      std::cout << "\n=== Detailed Results for Failed Tests ===\n";

      for (const auto& test_name : failed_tests_) {
        std::cout << "\nTest Case: " << test_name << "\n";

        // 输出失败的详细信息
        std::cout << "Failures:\n";
        for (const auto& failure : test_failures_[test_name]) {
          std::cout << failure << "\n";
        }

        // 输出函数的输出内容
        std::cout << "Output:\n";
        std::cout << test_outputs_[test_name] << std::flush;
      }
    }
    default_listener_->OnTestProgramEnd(unit_test);
  }

 private:
  ::testing::TestEventListener* default_listener_;
  std::vector<std::string> failed_tests_;                          // 保存失败测试的名称
  std::unordered_map<std::string, std::vector<std::string>> test_failures_;  // 失败测试的详细信息
  std::unordered_map<std::string, std::string> test_outputs_;      // 失败测试的输出内容
  std::unique_ptr<std::ostringstream> current_test_output_;        // 当前测试的输出流
  std::streambuf* original_cout_;                                  // 保存原始的 std::cout 缓冲区
  std::string current_test_name_;                                  // 当前测试的名称
};

// 注册监听器
void InstallFailureOnlyListener() {
  ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
  auto* default_listener = listeners.Release(listeners.default_result_printer());
  listeners.Append(new FailureOnlyEventListener(default_listener));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  InstallFailureOnlyListener();  // 安装自定义监听器
  return RUN_ALL_TESTS();
}