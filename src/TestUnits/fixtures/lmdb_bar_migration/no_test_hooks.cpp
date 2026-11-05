#include "../../../WtDataStorageAD/LMDBBarMigration.hpp"
int main()
{
    wt_ad_bar_migration::Bindings binding;
    auto result = wt_ad_bar_migration::stageBatch(binding,
        wt_ad_bar_key_v2::LegacySourceHost::LittleEndian,
        wt_ad_bar_key_v2::LegacyValueLayout::Aligned64, {}, 1);
    return result.code == wt_ad_bar_migration::ResultCode::UnpreparedBindings ? 0 : 1;
}
