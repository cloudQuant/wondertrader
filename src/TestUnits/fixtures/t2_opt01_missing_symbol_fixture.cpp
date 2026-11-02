// Deliberately exports no CTPOpt creator symbol. The parser must close the
// loaded module and report init failure when dlsym/GetProcAddress misses it.
extern "C" int t2_opt01_fixture_without_ctpopt_creator()
{
	return 1;
}
