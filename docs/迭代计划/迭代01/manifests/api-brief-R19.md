# API 简报 R19

测试目标: mqshare。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtMsgQue/MQServer.cpp` (249 行)
- L121: `SpinLocklock(m_mtxCast)`
- L140: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L148: `SpinLocklock(m_mtxCast)`
- L162: `SpinLocklock(m_mtxCast)`

### `src/WtMsgQue/MQClient.cpp` (199 行)
- L160: `std::this_thread::sleep_for(std::chrono::milliseconds(1))`

### `src/WtShareHelper/ShareBlocks.h` (214 行)
- 类型: _CmdBlock:91, ShareBlocks:107
- L119: `typedefvoid(*FuncLogger)(uint32_t, const char*)`
- L135: `boolinit_master(const char* name, const char* path = "")`
- L136: `boolinit_slave(const char* name, const char* path = "")`
- L138: `boolupdate_slave(const char* name, bool bForce)`
- L139: `boolrelease_slave(const char* name)`
- L141: `std::vector<std::string>get_sections(const char* domain)`
- L142: `std::vector<KeyInfo*>get_keys(const char* domain, const char* section)`
- L144: `uint64_tget_section_updatetime(const char* domain, const char* section)`
- L145: `boolcommit_section(const char* domain, const char* section)`
- L147: `booldelete_section(const char* domain, const char*section)`
- L149: `const char*allocate_string(const char* domain, const char* section, const char* key, const char* initVal = "", bool bForceWrite = false)`
- L150: `int32_t*allocate_int32(const char* domain, const char* section, const char* key, int32_t initVal = 0, bool bForceWrite = false)`
- L151: `int64_t*allocate_int64(const char* domain, const char* section, const char* key, int64_t initVal = 0, bool bForceWrite = false)`
- L152: `uint32_t*allocate_uint32(const char* domain, const char* section, const char* key, uint32_t initVal = 0, bool bForceWrite = false)`
- L153: `uint64_t*allocate_uint64(const char* domain, const char* section, const char* key, uint64_t initVal = 0, bool bForceWrite = false)`
- L154: `double*allocate_double(const char* domain, const char* section, const char* key, double initVal = 0, bool bForceWrite = false)`
- L156: `boolset_string(const char* domain, const char* section, const char* key, const char* val)`
- L157: `boolset_int32(const char* domain, const char* section, const char* key, int32_t val)`
- L158: `boolset_int64(const char* domain, const char* section, const char* key, int64_t val)`
- L159: `boolset_uint32(const char* domain, const char* section, const char* key, uint32_t val)`
- L160: `boolset_uint64(const char* domain, const char* section, const char* key, uint64_t val)`
- L161: `boolset_double(const char* domain, const char* section, const char* key, double val)`
- L163: `const char*get_string(const char* domain, const char* section, const char* key, const char* defVal = "")`
- L164: `int32_tget_int32(const char* domain, const char* section, const char* key, int32_t defVal = 0)`
- L165: `int64_tget_int64(const char* domain, const char* section, const char* key, int64_t defVal = 0)`
- L166: `uint32_tget_uint32(const char* domain, const char* section, const char* key, uint32_t defVal = 0)`
- L167: `uint64_tget_uint64(const char* domain, const char* section, const char* key, uint64_t defVal = 0)`
- L168: `doubleget_double(const char* domain, const char* section, const char* key, double defVal = 0)`
- L171: `boolinit_cmder(const char* name, bool isCmder = false, const char* path = "")`
- L172: `booladd_cmd(const char* name, const char* cmd)`
- L173: `const char*get_cmd(const char* name, uint32_t& lastIdx)`
- L176: `void*make_valid(const char* domain, const char* section, const char* key, ValueType vType, SecInfo* &secInfo)`
- L177: `void*check_valid(const char* domain, const char* section, const char* key, ValueType vType, SecInfo* &secInfo)`

### `src/WtMsgQue/MQClient.h` (162 行)
- 类型: MQManager:19, MQClient:27
- L49: `voidextract_buffer()`
- L76: `inline uint32_tid() const { return _id; }`
- L86: `boolinit(const char* url, FuncMQCallback cb)`
- L94: `voidstart()`

