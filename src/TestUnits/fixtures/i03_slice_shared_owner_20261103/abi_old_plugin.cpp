// Built ONLY against the pre-change (baseline) WTSDataDef.hpp. Emulates an external
// strategy plugin compiled with the old header that reads host-created slices through
// the old inline accessors, and creates slices with the old layout that the host reads.
#include "Includes/WTSDataDef.hpp"

USING_NS_WTP;

#define OLD_EXPORT extern "C" __attribute__((visibility("default")))

OLD_EXPORT uint32_t old_tick_size(WTSTickSlice* s) { return s->size(); }
OLD_EXPORT double old_tick_price(WTSTickSlice* s, int32_t idx) { const WTSTickStruct* t = s->at(idx); return t ? t->price : -1.0; }
OLD_EXPORT std::size_t old_tick_blocks(WTSTickSlice* s) { return s->get_block_counts(); }
OLD_EXPORT int32_t old_kline_size(WTSKlineSlice* s) { return s->size(); }
OLD_EXPORT double old_kline_close(WTSKlineSlice* s, int32_t idx) { WTSBarStruct* b = s->at(idx); return b ? b->close : -1.0; }
OLD_EXPORT double old_kline_max(WTSKlineSlice* s) { return s->maxprice(0, -1); }
OLD_EXPORT const char* old_kline_code(WTSKlineSlice* s) { return s->code(); }
OLD_EXPORT uint32_t old_trans_size(WTSTransSlice* s) { return s->size(); }
OLD_EXPORT double old_trans_price(WTSTransSlice* s, int32_t idx) { const WTSTransStruct* t = s->at(idx); return t ? t->price : -1.0; }
OLD_EXPORT void old_release(WTSObject* o) { o->release(); }
OLD_EXPORT void old_retain(WTSObject* o) { o->retain(); }
OLD_EXPORT WTSTickSlice* old_create_tick(WTSTickStruct* p, uint32_t n) { return WTSTickSlice::create("OLD", p, n); }
OLD_EXPORT std::size_t old_sizeof_tick_slice() { return sizeof(WTSTickSlice); }
OLD_EXPORT std::size_t old_sizeof_kline_slice() { return sizeof(WTSKlineSlice); }
