#include "WtBtPorter/WtBtPorter.h"
#include <cstring>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
const char* code = "SHFE.rb.2405";
std::string scenario;
WtReplayStatusV1 callbackStatus{};
std::atomic<int> callbackCalls{0};
int callbackRc = -9, initBars = -1, tickCalls = 0;
void bars(CtxHandler, const char*, const char*, WTSBarStruct*, WtUInt32, bool) {}
void strategy_init(CtxHandler id) {
 initBars = static_cast<int>(cta_get_bars(id, code, "d1", 2, true, bars));
 cta_sub_ticks(id, code);
 if (scenario == "init-throw") throw std::runtime_error("test preparation callback failure");
}
void strategy_tick(CtxHandler id, const char* contract, WTSTickStruct*) {
 ++tickCalls;
 if (scenario == "fee" && tickCalls == 1) cta_set_position(id, contract, 1, "status", 0, 0);
}
void event(WtUInt32 type, WtUInt32, WtUInt32) {
 if (type == EVENT_ENGINE_INIT && scenario == "stop") stop_backtest();
 if (type == EVENT_BACKTEST_END) {
  callbackStatus = {}; callbackStatus.struct_size = sizeof(callbackStatus); callbackStatus.abi_version = 1;
  callbackRc = get_backtest_status_v1(&callbackStatus);
  callbackCalls.fetch_add(1, std::memory_order_release);
  if (scenario == "callback-throw") throw std::runtime_error("test completion callback failure");
 }
}
void write(const std::filesystem::path& p, const char* text) { std::ofstream(p) << text; }
int fail(const std::string& text) { std::cerr << "FAIL " << text << "\n"; return 1; }
WtReplayStatusV1 snapshot() { WtReplayStatusV1 s{}; s.struct_size = sizeof(s); s.abi_version = 1; if (get_backtest_status_v1(&s) != 0) std::abort(); return s; }
}
int main(int argc, char** argv) {
 if (argc != 2) return fail("scenario missing");
 scenario = argv[1];
 if (scenario == "arguments") {
  if (get_backtest_status_v1(nullptr) != -1) return fail("null");
  WtReplayStatusV1 s{}; s.struct_size = sizeof(s) - 1; s.abi_version = 1;
  const WtReplayStatusV1 old = s;
  if (get_backtest_status_v1(&s) != -1 || std::memcmp(&s, &old, sizeof(s))) return fail("short buffer modified");
  s.struct_size = sizeof(s); s.abi_version = 2; const auto wrong = s;
  if (get_backtest_status_v1(&s) != -1 || std::memcmp(&s, &wrong, sizeof(s))) return fail("version modified");
  struct Extended { WtReplayStatusV1 status; unsigned char tail[32]; } extended;
  std::memset(&extended, 0xa5, sizeof(extended)); extended.status.struct_size = sizeof(extended); extended.status.abi_version = 1;
  if (get_backtest_status_v1(&extended.status) || extended.status.struct_size != sizeof(WtReplayStatusV1) || extended.status.abi_version != 1 || extended.status.state != WT_REPLAY_IDLE || extended.status.generation != 0 || extended.status.flags != 0 || std::memchr(extended.status.message, 0, sizeof(extended.status.message)) == nullptr) return fail("idle");
  for (unsigned char byte : extended.tail) if (byte != 0xa5) return fail("extension overwritten");
  std::cout << "PASS arguments idle\n"; return 0;
 }
 char name[] = "/tmp/wt-porter-status-XXXXXX"; char* root = mkdtemp(name); if (!root) return fail("mkdtemp");
 const std::filesystem::path dir(root); std::filesystem::create_directories(dir / "csv");
 write(dir / "sessions.json", R"({"TRADING":{"name":"all","sections":[{"from":0,"to":0}]}})");
 write(dir / "holidays.json", R"({"CHINA":[]})");
 write(dir / "commodities.json", R"({"SHFE":{"rb":{"name":"steel","session":"TRADING","holiday":"CHINA","pricetick":1,"volscale":10,"covermode":0,"pricemode":0,"trademode":0,"category":1}}})");
 write(dir / "contracts.json", R"({"SHFE":{"rb2405":{"name":"steel","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20300101}}})");
 write(dir / "csv/SHFE.rb.2405_d.csv", "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n20231229,150000,98,101,97,100,100,10000,0,0,0\n20240102,150000,100,103,99,102,100,10000,0,0,0\n20240103,150000,102,105,101,104,100,10000,0,0,0\n20240104,150000,104,107,103,106,100,10000,0,0,0\n20240105,150000,106,109,105,108,100,10000,0,0,0\n");
 const std::string p = dir.string();
 std::string cfg = "{\"replayer\":{\"mode\":\"csv\",\"path\":\"" + p + "\",\"stime\":202401020930,\"etime\":202401051500,\"tick\":false,\"basefiles\":{\"session\":\"" + p + "/sessions.json\",\"holiday\":\"" + p + "/holidays.json\",\"commodity\":\"" + p + "/commodities.json\",\"contract\":\"" + p + "/contracts.json\"}},\"env\":{\"mocker\":\"none\"}}";
 init_backtest(R"({"root":{"level":"debug","async":false,"sinks":[]}})", false, p.c_str());
 char err[512]{}; if (config_backtest_v2(cfg.c_str(), false, err, sizeof(err))) return fail(std::string("config ") + err);
 register_evt_callback(event); register_cta_callbacks(strategy_init, strategy_tick, nullptr, nullptr, nullptr, nullptr, nullptr);
 if (!init_cta_mocker("status", 0, false, false, false, false)) return fail("init mocker");
 const bool async = scenario == "normal-async" || scenario == "launch-failure";
 run_backtest(false, async);
 if (async) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (callbackCalls.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline)
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (callbackCalls.load(std::memory_order_acquire) == 0) { stop_backtest(); return fail("async timeout"); }
  stop_backtest();
 }
 auto result = snapshot();
 std::cout << "scenario=" << scenario << " bars=" << initBars << " ticks=" << tickCalls << " event=" << callbackCalls << " callback=" << callbackStatus.state << "/" << callbackStatus.error_code << "/" << callbackStatus.flags << " final=" << result.state << "/" << result.error_code << "/" << result.flags << " generation=" << result.generation << " message=" << result.message << "\n";
 if (initBars < 1) return fail("main bars unavailable");
 if (callbackCalls != 1 || callbackRc != 0) return fail("termination callback");
 if (result.generation == 0 || callbackStatus.generation != result.generation || callbackStatus.flags != 0 || !(result.flags & WT_REPLAY_STATUS_FINAL)) return fail("callback generation or final barrier");
 if (scenario != "callback-throw" && (callbackStatus.state != result.state || callbackStatus.error_code != result.error_code)) return fail("callback snapshot mismatch");
 if ((scenario == "normal" || scenario == "normal-async") && (result.state != WT_REPLAY_SUCCEEDED || result.error_code != WT_REPLAY_ERROR_NONE)) return fail("normal result");
 if (scenario == "fee" && (tickCalls < 1 || result.state != WT_REPLAY_FAILED || result.error_code != WT_REPLAY_ERROR_FEE || result.message[0] == 0)) return fail("fee result");
 if (scenario == "stop" && (result.state != WT_REPLAY_STOPPED || result.error_code != WT_REPLAY_ERROR_NONE)) return fail("stop result");
 if (scenario == "launch-failure" && (result.state != WT_REPLAY_FAILED || result.error_code != WT_REPLAY_ERROR_INTERNAL || tickCalls != 0 || result.message[0] == 0)) return fail("launch result");
 if (scenario == "init-throw" && (result.state != WT_REPLAY_FAILED || result.error_code != WT_REPLAY_ERROR_CALLBACK_EXCEPTION || tickCalls != 0)) return fail("preparation exception result");
 if (scenario == "callback-throw" && (callbackStatus.state != WT_REPLAY_SUCCEEDED || result.state != WT_REPLAY_FAILED || result.error_code != WT_REPLAY_ERROR_CALLBACK_EXCEPTION)) return fail("callback exception result");
 if (scenario == "normal") {
  const auto previous = result;
  run_backtest(false, false); result = snapshot();
  if (callbackCalls.load(std::memory_order_acquire) != 2 || result.state != WT_REPLAY_SUCCEEDED ||
      result.generation <= previous.generation || callbackStatus.generation != result.generation || callbackStatus.flags != 0 || !(result.flags & WT_REPLAY_STATUS_FINAL) ||
      previous.generation == result.generation) return fail("second generation");
 }
 release_backtest(); std::error_code ec; std::filesystem::remove_all(dir, ec);
 std::cout << "PASS " << scenario << "\n"; return 0;
}
