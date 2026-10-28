# API 简报 R5

测试目标: dtcore。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtDtCore/StateMonitor.cpp` (543 行)
- L52: `WTSLogger::error("Loading state config failed")`
- L68: `StatePtrstateInfo(new StateInfo)`
- L168: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`

### `src/WtDtCore/UDPCaster.cpp` (417 行)
- 类型: UDPDataPacket:38
- L106: `boost::asio::socket_base::broadcastoption(true)`
- L227: `UDPReceiverPtritem(new UDPReceiver(EndPoint(addr, port), type))`
- L249: `UDPReceiverPtritem(new UDPReceiver(EndPoint(addr, port), type))`
- L250: `UDPSocketPtrsock(new UDPSocket(m_ioservice, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), sendport)))`
- L251: `boost::asio::ip::multicast::join_groupoption(item->_ep.address())`
- L294: `StdUniqueLocklock(m_mtxCast)`
- L306: `StdUniqueLocklock(m_mtxCast)`
- L313: `StdUniqueLocklock(m_mtxCast)`

