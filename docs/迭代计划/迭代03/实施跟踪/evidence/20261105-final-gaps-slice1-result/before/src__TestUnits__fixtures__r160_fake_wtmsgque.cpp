#include "WtMsgQue/WtMsgQue.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
constexpr WtUInt32 kServerId = 0xF1234567u;
std::mutex g_mutex;
std::atomic<uint32_t> g_create_count{ 0 };
std::atomic<uint32_t> g_destroy_count{ 0 };
std::atomic<uint32_t> g_publish_count{ 0 };
std::atomic<uint32_t> g_register_count{ 0 };
std::atomic<WtUInt32> g_last_server_id{ 0 };
std::atomic<WtUInt32> g_destroyed_id{ 0 };
std::atomic<WtUInt32> g_published_id{ 0 };
std::atomic<WtUInt32> g_data_len{ 0 };
std::atomic<uint64_t> g_data_size{ 0 };
std::atomic<bool> g_confirm{ true };
std::string g_url;
std::string g_topic;
FuncLogCallback g_log_callback = nullptr;
}

extern "C"
{
	EXPORT_FLAG void r160_fake_reset()
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_url.clear();
			g_topic.clear();
			g_log_callback = nullptr;
		}
		g_create_count.store(0);
		g_destroy_count.store(0);
		g_publish_count.store(0);
		g_register_count.store(0);
		g_last_server_id.store(0);
		g_destroyed_id.store(0);
		g_published_id.store(0);
		g_data_len.store(0);
		g_data_size.store(0);
		g_confirm.store(true);
	}

	EXPORT_FLAG void regiter_callbacks(FuncLogCallback cbLog)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_log_callback = cbLog;
		++g_register_count;
	}

	EXPORT_FLAG WtUInt32 create_server(const char* url, bool confirm)
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_url = url == nullptr ? "" : url;
		}
		g_confirm.store(confirm);
		++g_create_count;
		g_last_server_id.store(kServerId);
		return kServerId;
	}

	EXPORT_FLAG void destroy_server(WtUInt32 id)
	{
		g_destroyed_id.store(id);
		++g_destroy_count;
	}

	EXPORT_FLAG void publish_message(WtUInt32 id, const char* topic, const char* data, WtUInt32 dataLen)
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_topic = topic == nullptr ? "" : topic;
		}
		g_published_id.store(id);
		g_data_len.store(dataLen);
		g_data_size.store(data == nullptr ? 0 : std::strlen(data));
		++g_publish_count;
	}

	EXPORT_FLAG uint32_t r160_fake_create_count() { return g_create_count.load(); }
	EXPORT_FLAG uint32_t r160_fake_destroy_count() { return g_destroy_count.load(); }
	EXPORT_FLAG uint32_t r160_fake_publish_count() { return g_publish_count.load(); }
	EXPORT_FLAG uint32_t r160_fake_register_count() { return g_register_count.load(); }
	EXPORT_FLAG WtUInt32 r160_fake_server_id() { return g_last_server_id.load(); }
	EXPORT_FLAG WtUInt32 r160_fake_destroyed_id() { return g_destroyed_id.load(); }
	EXPORT_FLAG WtUInt32 r160_fake_published_id() { return g_published_id.load(); }
	EXPORT_FLAG WtUInt32 r160_fake_data_len() { return g_data_len.load(); }
	EXPORT_FLAG uint64_t r160_fake_data_size() { return g_data_size.load(); }
	EXPORT_FLAG bool r160_fake_confirm() { return g_confirm.load(); }
	EXPORT_FLAG const char* r160_fake_url()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_url.c_str();
	}
	EXPORT_FLAG const char* r160_fake_topic()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_topic.c_str();
	}
}
