#include <atomic>
#include <type_traits>
#include <boost/asio/io_context.hpp>
#include "Includes/WTSMarcos.h"
#include "Includes/WTSObject.hpp"
#include "Share/StdUtils.hpp"
#define private public
#include "WtCore/EventNotifier.h"
#undef private
static_assert(std::is_same<decltype(wtp::EventNotifier::_stopped), std::atomic<bool>>::value, "worker stop flag must be atomic");
