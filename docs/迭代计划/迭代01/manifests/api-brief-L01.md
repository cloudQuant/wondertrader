# API 简报 L01

覆盖模块: Share, WTSUtils。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `Share`

### `src/Share/BoostFile.hpp` (209 行)
- 类型: BoostFile:32
- L80: `bip::ipcdetail::close_file(_handle)`

### `src/Share/BoostMappingFile.hpp` (222 行)
- 类型: BoostMappingFile:24

### `src/Share/BoostShm.hpp` (224 行)
- 类型: BoostShm:31
- L157: `boost::interprocess::shared_memory_object::remove(name)`

### `src/Share/CodeHelper.hpp` (663 行)
- 类型: CodeHelper:39
- L69: `inline boolisExright() const { return _exright != 0; }`
- L72: `inline boolisForwardAdj() const { return _exright == 1; }`
- L75: `inline boolisBackwardAdj() const { return _exright == 2; }`
- L452: `StrUtil::replace(s, "-", ".")`
- L521: `StrUtil::replace(ret, ".", "-")`
- L523: `StrUtil::replace(ret, ".", "")`

### `src/Share/Converter.hpp` (71 行)
- L25: `std::to_chars(buffer, buffer + 64, val)`
- L32: `std::to_chars(buffer, buffer + len, val)`
- L40: `std::from_chars(str, str + strlen(str), ret)`
- L48: `std::from_chars(str.begin(), str.end(), ret)`

### `src/Share/CpuHelper.hpp` (60 行)
- 类型: CpuHelper:8

### `src/Share/DLLHelper.hpp` (90 行)
- 类型: DLLHelper:23
- L79: `std::stringret(name, idx)`

### `src/Share/IniHelper.hpp` (420 行)
- 类型: IniHelper:34
- L78: `boost::property_tree::ini_parser::read_ini(szFile, _root)`
- L98: `boost::property_tree::ini_parser::write_ini(filename, _root)`
- L100: `boost::property_tree::ini_parser::write_ini(_fname.c_str(), _root)`
- L108: `inline boolisLoaded() const{ return _loaded; }`

### `src/Share/ModuleHelper.hpp` (97 行)
- L48: `voidinst_hlp() }`

### `src/Share/ObjectPool.hpp` (87 行)
- 类型: ObjectPool:22
- L44: `virtual ~ObjectPool() }`

### `src/Share/ProcHelper.hpp` (36 行)
- 类型: ProcHelper:11

### `src/Share/SpinMutex.hpp` (67 行)
- 类型: SpinMutex:23, SpinLock:55
- L41: `asmvolatile("yield" ::: "memory")`

### `src/Share/StdUtils.hpp` (147 行)
- 类型: StdLocker:35, StdFile:54
- L62: `throw std::runtime_error("File does not exist")`
- L71: `throw std::runtime_error("Offset exceeds file size")`
- L87: `std::ifstreamfile(filename, std::ios::binary)`
- L90: `throw std::runtime_error("Failed to open file")`
- L107: `throw std::runtime_error("Failed to read expected number of bytes")`
- L117: `std::ofstreamfile(filename, std::ios::binary)`
- L120: `throw std::runtime_error("Failed to open file for writing")`
- L128: `std::ofstreamfile(filename, std::ios::binary)`
- L131: `throw std::runtime_error("Failed to open file for writing")`

### `src/Share/StrUtil.hpp` (340 行)
- L55: `std::stringpattern(chars)`
- L310: `std::replace(path.begin(), path.end(), '\\', '/')`

### `src/Share/TimeUtils.hpp` (481 行)
- 类型: KUSER_SHARED_DATA:44, tm:218, tm:267, Time32:371, tm:433, Ticker:437

### `src/Share/WtKVCache.hpp` (277 行)
- 类型: WtKVCache:22

### `src/Share/charconv.hpp` (670 行)
- 类型: UTF8toChar:43, ChartoUTF8:214, URLEncode:388, URLDecode:469, EncodingHelper:543
- L560: `static boolisGBK(unsigned char* data, std::size_t len)`
- L601: `static intpreNUm(unsigned char byte)`
- L635: `static boolisUtf8(unsigned char* data, std::size_t len)`

### `src/Share/cppcli.hpp` (1981 行)
- 类型: Option:107, Rule:108, pathUtil:170, algoUtil:231, Rule:542, detail:548, HelpDocStruct:556, Option:1018, detail:1026
- L92: `std::unique_lock<std::mutex>lock(_coutMutex)`
- L182: `static intreplace_all(std::string &str, const std::string &pattern, const std::string &newpat)`
- L193: `static std::stringgetFilename(const std::string &filePath)`
- L203: `static std::stringgetFilenameWithOutSuffix(const std::string &filePath)`
- L213: `static std::stringgetFileSuffix(const std::string &filePath)`
- L223: `static std::stringgetFileDir(const std::string &filePath)`
- L243: `static voidInitCommandMap(int length, char *strArr[], std::map<std::string, std::string> &stringMap)`
- L253: `static boolisInt(const std::string &value)`
- L263: `static boolisDouble(const std::string &value)`
- L273: `static boolverifyDouble(const std::string &value)`
- L315: `std::stringfilePathCopy(filePath)`
- L372: `std::stringfilePathCopy(filePath)`
- L404: `std::stringtheStr(strArr[currentIndex])`
- L629: `Rule *limitInt() /// valid after setting input type`
- L638: `Rule *limitDouble()`
- L647: `Rule *asHelpParam()`
- L656: `boolexists()`
- L769: `const std::stringgetError(cppcli::detail::ErrorEventType errorEventType)`
- L778: `std::stringbuildHelpInfoLine()`
- L788: `std::stringdebugInfo() const`
- L826: `bool cppcli::Rule::exists() return _existsInMap; }`
- L856: `oss << std::move(" NUMBER (DOUBLE) ")`
- L858: `oss << std::move(" NUMBER (INT) ")`
- L908: `oss << std::setw(commandsDis) << std::left << (_longParam.empty() ? _shortParam : _shortParam + " | " + _longParam)`
- L928: `oss << std::setw(helpInfoDis) << std::setw(helpInfoDis) << _helpInfo.substr(pos, helpInfoDis - theDis)`
- L1039: `static intnecessaryVerify(Option &opt)`
- L1049: `static intvalueTypeVerify(Option &opt)`
- L1059: `static intnumRangeVerify(Option &opt)`
- L1069: `static intoneOfVerify(Option &opt)`
- L1132: `voidparse()`
- L1142: `boolexists(const std::string shortParam)`
- L1152: `boolexists(const cppcli::Rule *rule)`
- L1164: `voidprintCommandMap()`
- L1174: `const std::stringgetWorkPath()`
- L1183: `const std::stringgetExecPath()`
- L1227: `voidrulesGainInputValue()`
- L1237: `std::stringgetInputValue(const cppcli::Rule &rule)`
- L1246: `std::stringbuildHelpDoc()`
- L1253: `voidprintHelpDoc()`
- L1263: `boolmapExists(const cppcli::Rule *rule)`
- L1273: `voidpathInit(int argc, char *argv[])`
- L1308: `std::exit(-1)`
- L1322: `std::exit(-1)`
- L1336: `std::exit(-1)`
- L1580: `std::cout <<buildHelpDoc()`
- L1582: `std::exit(0)`
- L1612: `cppcli::detail::algoUtil::InitCommandMap(argc, argv, _commandMap)`
- L1655: `std::exit(-1)`
- L1660: `std::exit(-1)`
- L1688: `std::exit(-1)`
- L1693: `std::exit(-1)`
- L1898: `std::cout <<buildHelpDoc()`
- L1899: `std::exit(0)`
- L1907: `const std::string cppcli::Option::getWorkPath() return _workPath; }`
- L1914: `const std::string cppcli::Option::getExecPath() return _execPath; }`

### `src/Share/fmt_enum_compat.h` (27 行)
- 类型: fmt:8, fmt:20

## `WTSUtils`

### `src/WTSUtils/WTSCfgLoader.cpp` (328 行)
- L304: `StdFile::read_file_content(filename, content)`

### `src/WTSUtils/WTSCfgLoader.h` (91 行)
- 类型: WTSVariant:19, WTSCfgLoader:31
- L41: `static WTSVariant*load_from_json(const char* content)`
- L51: `static WTSVariant*load_from_yaml(const char* content)`
- L64: `static WTSVariant*load_from_file(const char* filename)`
- L76: `static WTSVariant*load_from_content(const std::string& content, bool isYaml = false)`

### `src/WTSUtils/WTSCmpHelper.hpp` (71 行)
- 类型: WTSCmpHelper:26
- L67: `throw std::runtime_error("uncompressed data size does not match calculated data size")`

### `src/WTSUtils/WtLMDB.hpp` (400 行)
- 类型: WtLMDB:25, WtLMDBQuery:104
- L45: `inline MDB_env*env() const{ return _env; }`
- L46: `inline MDB_dbidbi() const { return _dbi; }`
- L86: `inline voidupdate_errno(int error) _errno = error; }`
- L88: `inline boolhas_error() const { return _errno != MDB_SUCCESS; }`
- L90: `inline boolis_readonly() const { return _readonly; }`
- L319: `std::reverse(ayKeys.begin(), ayKeys.end())`
- L320: `std::reverse(ayVals.begin(), ayVals.end())`

