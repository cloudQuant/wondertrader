// Micro-benchmark: slice creation/release cost. Built twice: against the baseline header
// (no owner API) and against the working tree (WT_I03_GREEN, owner API). Prints ns/op.
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>
#include <algorithm>

#include "Includes/WTSDataDef.hpp"

USING_NS_WTP;

template <typename F>
static double ns_per_op(F&& f, int iters)
{
	std::vector<double> samples;
	for (int r = 0; r < 7; r++)
	{
		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < iters; i++)
			f(i);
		auto t1 = std::chrono::steady_clock::now();
		samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / iters);
	}
	std::sort(samples.begin(), samples.end());
	return samples[samples.size() / 2];
}

static volatile double g_sink = 0;

int main()
{
	const int N = 2000000;
	auto ticks = std::make_shared<std::vector<WTSTickStruct>>(1024);
	auto dtls = std::make_shared<std::vector<WTSOrdDtlStruct>>(1024);
	for (auto& t : *ticks) t.price = 1.0;

	double tick = ns_per_op([&](int i) {
		WTSTickSlice* s = WTSTickSlice::create("SSE.STK.600000", ticks->data() + (i & 511), 256);
		g_sink = g_sink + s->at(-1)->price;
		s->release();
	}, N);
	double tick2 = ns_per_op([&](int i) {
		WTSTickSlice* s = WTSTickSlice::create("SSE.STK.600000", ticks->data() + (i & 511), 128);
		s->appendBlock(ticks->data() + 600, 128);
		g_sink = g_sink + s->at(-1)->price;
		s->release();
	}, N);
	double dtl = ns_per_op([&](int i) {
		WTSOrdDtlSlice* s = WTSOrdDtlSlice::create("SSE.STK.600000", dtls->data() + (i & 511), 256);
		g_sink = g_sink + s->at(-1)->price;
		s->release();
	}, N);
	std::printf("variant=%s tick_create_release_ns=%.2f tick_2block_ns=%.2f orddtl_create_release_ns=%.2f\n",
#if defined(WT_I03_GREEN)
		"new_no_owner",
#else
		"baseline",
#endif
		tick, tick2, dtl);
#if defined(WT_I03_GREEN)
	WTSSliceOwnerPtr tickOwner = ticks, dtlOwner = dtls;
	auto other = std::make_shared<int>(1);
	double tickO = ns_per_op([&](int i) {
		WTSTickSlice* s = WTSTickSlice::create("SSE.STK.600000", ticks->data() + (i & 511), 256);
		s->addOwner(tickOwner);
		g_sink = g_sink + s->at(-1)->price;
		s->release();
	}, N);
	double tick2O = ns_per_op([&](int i) {
		WTSTickSlice* s = WTSTickSlice::create("SSE.STK.600000", ticks->data() + (i & 511), 128);
		s->addOwner(tickOwner);
		s->appendBlock(ticks->data() + 600, 128);
		s->addOwner(other);
		g_sink = g_sink + s->at(-1)->price;
		s->release();
	}, N);
	double dtlO = ns_per_op([&](int i) {
		WTSOrdDtlSlice* s = WTSOrdDtlSlice::create("SSE.STK.600000", dtls->data() + (i & 511), 256);
		s->addOwner(dtlOwner);
		g_sink = g_sink + s->at(-1)->price;
		s->release();
	}, N);
	WTSHisTickData* obj = WTSHisTickData::create("X", (unsigned int)4);
	double objO = ns_per_op([&](int i) {
		WTSTickSlice* s = WTSTickSlice::create("SSE.STK.600000", ticks->data() + (i & 511), 256);
		s->addOwner(makeSliceOwner(obj));
		g_sink = g_sink + s->at(-1)->price;
		s->release();
	}, N);
	obj->release();
	std::printf("variant=new_with_owner tick_create_release_ns=%.2f tick_2block_ns=%.2f orddtl_create_release_ns=%.2f tick_wtsobject_owner_ns=%.2f\n",
		tickO, tick2O, dtlO, objO);
#endif
	return 0;
}
