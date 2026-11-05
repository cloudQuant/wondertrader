#include "WTSUtils/WTSCfgLoader.h"
#include "Includes/WTSVariant.hpp"
#include <string>
int main()
{
    WTSVariant* obj = WTSCfgLoader::load_from_normalized_content("{\"answer\":42}", false);
    if (!obj) return 1;
    const bool ok = obj->getInt32("answer") == 42;
    obj->release();
    return ok ? 0 : 2;
}
