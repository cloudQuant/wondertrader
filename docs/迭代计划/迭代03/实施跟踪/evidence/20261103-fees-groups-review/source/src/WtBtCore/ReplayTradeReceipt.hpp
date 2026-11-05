#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <cmath>
#include "../Share/FeeSchedule.hpp"

// A per-thread stack allows callbacks on other mockers without losing the
// active receipt. Same-mocker nested fills are rejected while a plan is live.
namespace replay_trade {
inline bool fundFinite(double profit, double dynamicProfit, double fees) {
 const double gross = profit + dynamicProfit;
 return std::isfinite(profit) && std::isfinite(dynamicProfit) && std::isfinite(fees) &&
  std::isfinite(gross) && std::isfinite(gross - fees);
}
inline bool appendFee(const wt_fee::Context& context, double price, double qty,
 uint32_t offset, double& total, std::vector<double>& fees) {
 const auto fee = wt_fee::calculate(context, price, qty, offset);
 if (fee.status != wt_fee::CalculationStatus::ok || !std::isfinite(total + fee.value)) return false;
 total += fee.value;
 fees.push_back(fee.value);
 return true;
}
struct Frame {
 Frame* previous = nullptr;
 const void* owner = nullptr;
 std::string code;
 uint32_t localid = 0;
 bool direction = false;
 uint32_t offset = 0;
 double quantity = 0;
 double price = 0;
 const double* remaining = nullptr;
 double expectedRemaining = 0;
 const std::vector<double>* fees = nullptr;
 bool expected = false;
 bool committed = false;
 bool rejected = false;
 std::function<bool()> stateValid;
};
inline thread_local Frame* top = nullptr;
inline Frame* find(const void* owner) {
 for (Frame* f = top; f; f = f->previous) if (f->owner == owner) return f;
 return nullptr;
}
struct Scope {
 Frame& frame;
 explicit Scope(Frame& f) : frame(f) { frame.previous = top; top = &frame; }
 ~Scope() { top = frame.previous; }
 Scope(const Scope&) = delete;
 Scope& operator=(const Scope&) = delete;
};
}
