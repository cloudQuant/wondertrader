#include "WtDtCore/DataManager.h"
// StateMonitor reaches the data manager only after CLOSED; R5 never does.
void DataManager::transHisData(const char*) {}
bool DataManager::isSessionProceeded(const char*) { return true; }
