#include <Includes/IRdmDtReader.h>

extern "C" EXPORT_FLAG wtp::IRdmDtReader* createRdmDtReader()
{
	return nullptr;
}

extern "C" EXPORT_FLAG void deleteRdmDtReader(wtp::IRdmDtReader* reader)
{
	(void)reader;
}
