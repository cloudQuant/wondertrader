#pragma once

#if defined(WT_DATA_WRITER_TEST_HOOKS)

#include <cstdint>
#include <memory>

class WtDataWriter;

namespace p10_writer_test_hooks
{
	struct Hooks
	{
		void* context = nullptr;
		void (*after_order_detail_insert)(void*) noexcept = nullptr;
		void (*snapshot_map_lock_contended)(void*) noexcept = nullptr;
		void (*after_order_details_copied)(void*, long, std::weak_ptr<void>) noexcept = nullptr;
		bool (*fail_next_rt_resize_map)(void*, bool old_mapping_retained) noexcept = nullptr;
		bool (*simulate_next_rt_resize_short_write)(void*, const char*, uint64_t, uint64_t) noexcept = nullptr;
		bool skip_initial_check_wait = false;
		bool (*fail_next_tick_cache_resize_map)(void*, bool old_mapping_retained) noexcept = nullptr;
		// Card 13 multi-writer crash points (appended; existing fields unchanged).
		// Called with the cross-process publish lock held, after the record payload is
		// written into the first unpublished slot and before the size release-store.
		void (*before_rt_publish)(void*, const char* path, uint32_t slot) noexcept = nullptr;
		// Called inside resizeRTBlock after the file has been extended and before the new
		// capacity is published (journal is in PREPARING state).
		void (*after_rt_extend_before_capacity)(void*, const char* path) noexcept = nullptr;
	};

	bool install(::WtDataWriter* writer, const Hooks& hooks) noexcept;
	void uninstall(::WtDataWriter* writer) noexcept;
}

#endif
