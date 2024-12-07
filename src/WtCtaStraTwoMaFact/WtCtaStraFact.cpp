#include "WtCtaStraFact.h"
#include "WtStraTwoMaStrategy.h"

#include <string.h>
#include <boost/config.hpp>
#include <iostream>

const char* FACT_NAME = "TwoMaStrategy";


extern "C"
{
	EXPORT_FLAG ICtaStrategyFact* createStrategyFact()
	{
		ICtaStrategyFact* fact = new WtStraFact();
		return fact;
	}

	EXPORT_FLAG void deleteStrategyFact(ICtaStrategyFact* fact)
	{
		if (fact != NULL)
			delete fact;
	}
};


WtStraFact::WtStraFact()
{
}


WtStraFact::~WtStraFact()
{
}

CtaStrategy* WtStraFact::createStrategy(const char* name, const char* id)
{
    // std::cout << "WtCtastraFact  name = " << name << " " << "id = " << id << std::endl;
	if (strcmp(name, "TwoMaStrategy") == 0)
		return new WtStraTwoMaStrategy(id);

	return NULL;
}

bool WtStraFact::deleteStrategy(CtaStrategy* stra)
{
	if (stra == NULL)
		return true;

	if (strcmp(stra->getFactName(), FACT_NAME) != 0)
		return false;

	delete stra;
	return true;
}

void WtStraFact::enumStrategy(FuncEnumStrategyCallback cb)
{
	cb(FACT_NAME, "TwoMaStrategy", false);
	cb(FACT_NAME, "PairTradingFci", false);
	cb(FACT_NAME, "CtaXPA", true);
}

const char* WtStraFact::getName()
{
	return FACT_NAME;
}