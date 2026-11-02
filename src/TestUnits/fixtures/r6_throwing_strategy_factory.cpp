#include <stdexcept>

extern "C" void* createStrategyFact()
{
	throw std::runtime_error("R6 factory creator failure");
}

extern "C" void* createSelStrategyFact()
{
	throw std::runtime_error("R6 SEL factory creator failure");
}

extern "C" void deleteSelStrategyFact(void*)
{
}
