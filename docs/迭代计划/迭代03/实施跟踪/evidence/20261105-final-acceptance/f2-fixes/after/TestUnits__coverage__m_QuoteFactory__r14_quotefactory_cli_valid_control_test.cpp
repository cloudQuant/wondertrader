/*
 * R14-004 CLI positive control: launch the actual QuoteFactory executable with
 * a real local WtDataStorage writer, then request its documented signal
 * shutdown and verify successful writer startup plus process status.
 *
 * The fixture uses allday mode, no casters, and an empty parser list. It copies
 * the local writer module byte-for-byte into a disposable temporary directory;
 * all writer files stay there and normal process-exit cleanup stops its thread.
 */
#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "Share/DLLHelper.hpp"

namespace {

namespace fs = std::filesystem;
constexpr const char* kReadyMarker = "0 market data parsers loaded in total";

struct QfCliResult
{
	bool exited = false;
	bool startup_seen = false;
	bool stop_sent = false;
	int code = -1;
	int signal = -1;
	std::string output;
};

class QfCliTempDirectory
{
public:
	QfCliTempDirectory()
	{
		const std::string pattern = (fs::temp_directory_path() / "r14-qf-cli-XXXXXX").string();
		std::vector<char> writable(pattern.begin(), pattern.end());
		writable.push_back('\0');
		char* created = mkdtemp(writable.data());
		if (created != nullptr)
			_path = created;
	}

	~QfCliTempDirectory()
	{
		if (!_path.empty())
			fs::remove_all(_path);
	}

	const fs::path& path() const { return _path; }

private:
	fs::path _path;
};

bool write_qf_cli_file(const fs::path& path, const std::string& contents)
{
	std::ofstream output(path);
	output << contents;
	return output.good();
}

bool copy_qf_module_and_verify(const fs::path& source, const fs::path& destination)
{
	std::error_code ec;
	if (!fs::is_regular_file(source, ec) || ec)
		return false;
	const std::uintmax_t sourceSize = fs::file_size(source, ec);
	if (ec || !fs::copy_file(source, destination, ec) || ec)
		return false;
	if (fs::is_symlink(destination, ec) || ec || !fs::is_regular_file(destination, ec) || ec ||
		fs::file_size(destination, ec) != sourceSize || ec)
		return false;

	std::ifstream sourceStream(source, std::ios::binary);
	std::ifstream copyStream(destination, std::ios::binary);
	if (!sourceStream || !copyStream)
		return false;
	const std::vector<char> sourceBytes((std::istreambuf_iterator<char>(sourceStream)),
		std::istreambuf_iterator<char>());
	const std::vector<char> copyBytes((std::istreambuf_iterator<char>(copyStream)),
		std::istreambuf_iterator<char>());
	return !sourceStream.bad() && !copyStream.bad() && sourceBytes == copyBytes &&
		static_cast<std::uintmax_t>(sourceBytes.size()) == sourceSize;
}

void append_qf_cli_output(int fd, std::string& output)
{
	char buffer[2048];
	for (;;)
	{
		const ssize_t count = read(fd, buffer, sizeof(buffer));
		if (count <= 0)
			return;
		output.append(buffer, static_cast<size_t>(count));
	}
}

void qf_test_binary_marker() {}

fs::path qf_test_binary_path()
{
	Dl_info info{};
	if (dladdr(reinterpret_cast<void*>(&qf_test_binary_marker), &info) == 0 || info.dli_fname == nullptr)
		return {};
	return fs::absolute(info.dli_fname);
}

QfCliResult run_product_qf_cli(const fs::path& executable, const fs::path& cwd,
	const fs::path& config, const fs::path& log_config)
{
	QfCliResult result;
	int fds[2];
	if (pipe(fds) != 0)
		return result;

	const pid_t pid = fork();
	if (pid < 0)
	{
		close(fds[0]);
		close(fds[1]);
		return result;
	}

	if (pid == 0)
	{
		close(fds[0]);
		(void)dup2(fds[1], STDOUT_FILENO);
		(void)dup2(fds[1], STDERR_FILENO);
		close(fds[1]);
		if (chdir(cwd.c_str()) != 0)
			_exit(120);

		std::vector<std::string> words = {
			executable.string(), "-c", config.string(), "-l", log_config.string()
		};
		std::vector<char*> argv;
		argv.reserve(words.size());
		for (std::string& word : words)
			argv.push_back(word.data());
		execv(executable.c_str(), argv.data());
		_exit(121);
	}

	close(fds[1]);
	const int old_flags = fcntl(fds[0], F_GETFL, 0);
	if (old_flags < 0 || fcntl(fds[0], F_SETFL, old_flags | O_NONBLOCK) != 0)
	{
		(void)kill(pid, SIGKILL);
		int ignored_status = 0;
		(void)waitpid(pid, &ignored_status, 0);
		close(fds[0]);
		return result;
	}

	int status = 0;
	bool child_reaped = false;
	// I03-NEW-136: the exec'ed QuoteFactory can need well over 5 s to reach parser startup
	// under ASan and a loaded machine (marker arrived after the old budget). Wait budget only.
	const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (std::chrono::steady_clock::now() < startup_deadline && !result.startup_seen)
	{
		pollfd event = { fds[0], POLLIN | POLLHUP, 0 };
		(void)poll(&event, 1, 100);
		append_qf_cli_output(fds[0], result.output);
		result.startup_seen = result.output.find(kReadyMarker) != std::string::npos;

		const pid_t waited = waitpid(pid, &status, WNOHANG);
		if (waited == pid)
		{
			child_reaped = true;
			break;
		}
	}

	if (result.startup_seen && !child_reaped)
	{
		// The parser-config record is near the end of initialize(); allow main to
		// install its signal hook and enter the wait loop before requesting stop.
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		const pid_t waited = waitpid(pid, &status, WNOHANG);
		if (waited == 0 && kill(pid, SIGINT) == 0)
			result.stop_sent = true;
		else if (waited == pid)
			child_reaped = true;
	}

	if (!child_reaped)
	{
		const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (std::chrono::steady_clock::now() < stop_deadline)
		{
			append_qf_cli_output(fds[0], result.output);
			const pid_t waited = waitpid(pid, &status, WNOHANG);
			if (waited == pid)
			{
				child_reaped = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	if (!child_reaped)
	{
		(void)kill(pid, SIGKILL);
		if (waitpid(pid, &status, 0) == pid)
			child_reaped = true;
	}

	append_qf_cli_output(fds[0], result.output);
	close(fds[0]);
	if (child_reaped)
	{
		result.exited = WIFEXITED(status);
		result.code = result.exited ? WEXITSTATUS(status) : -1;
		result.signal = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
	}
	return result;
}

} // namespace

TEST(r14_quote_cli, valid_config_with_real_writer_returns_zero_after_signal)
{
	QfCliTempDirectory temp;
	ASSERT_FALSE(temp.path().empty());
	const fs::path test_binary = qf_test_binary_path();
	ASSERT_FALSE(test_binary.empty());
	const fs::path bin_dir = test_binary.parent_path().parent_path();
	const std::string writer_module = DLLHelper::wrap_module("WtDataStorage");
	const fs::path writer_library = bin_dir / writer_module;
	ASSERT_TRUE(fs::exists(writer_library)) << writer_library.string();

	const char* product_override = std::getenv("WT_R14_QF_PRODUCT_BIN");
	const fs::path executable = product_override == nullptr
		? bin_dir / "QuoteFactory" / "QuoteFactory"
		: fs::path(product_override);
	ASSERT_TRUE(fs::exists(executable)) << "QuoteFactory product executable not found: " << executable.string();

	const fs::path staged_writer = temp.path() / writer_module;
	ASSERT_TRUE(copy_qf_module_and_verify(writer_library, staged_writer))
		<< "cannot copy and verify local WtDataStorage in temporary cwd";
	ASSERT_TRUE(fs::is_regular_file(staged_writer));
	ASSERT_FALSE(fs::is_symlink(staged_writer));

	const fs::path data_dir = temp.path() / "data";
	const fs::path config_path = temp.path() / "valid.yaml";
	const fs::path log_path = temp.path() / "console-log.yaml";
	ASSERT_TRUE(write_qf_cli_file(config_path,
		"basefiles: {}\n"
		"allday: true\n"
		"writer:\n"
		"    module: WtDataStorage\n"
		"    path: " + data_dir.string() + "/\n"
		"    async: false\n"
		"    disablehis: true\n"
		"    disabletick: true\n"
		"    disablemin1: true\n"
		"    disablemin5: true\n"
		"    disableday: true\n"
		"    disabletrans: true\n"
		"    disableordque: true\n"
		"    disableorddtl: true\n"
		"parsers: []\n"));
	ASSERT_TRUE(write_qf_cli_file(log_path,
		"root:\n"
		"    async: false\n"
		"    level: info\n"
		"    sinks:\n"
		"    -   type: console_sink\n"
		"        pattern: '%v'\n"));

	const QfCliResult result = run_product_qf_cli(executable, temp.path(), config_path, log_path);
	ASSERT_TRUE(result.startup_seen) << "CLI did not reach parser startup; output=" << result.output;
	ASSERT_TRUE(result.stop_sent) << "parent did not deliver the requested SIGINT; output=" << result.output;
	ASSERT_TRUE(result.exited) << "CLI stopped by signal " << result.signal << "; output=" << result.output;
	EXPECT_EQ(result.code, 0) << result.output;
	EXPECT_NE(result.output.find("WtDataWriter initialized"), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("Data storage module "), std::string::npos) << result.output;
	EXPECT_NE(result.output.find("Exit flag is true"), std::string::npos) << result.output;
	EXPECT_TRUE(fs::exists(data_dir / "cache.dmb")) << "real writer did not initialize its cache under the temp root";
}

