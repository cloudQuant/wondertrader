// Q03-005 / I02-NEW-067: live slices and reloading after cache eviction.
// These are known-red regression tests until slice ownership is implemented.
#include "q03_storage3_fixture.h"
#include "WtDataStorage/WtRdmDtReader.h"

namespace q03 {
class Q03RdmClearLifetime : public ::testing::Test {
protected:
 void SetUp() override { sandbox.reset(new Sandbox("rdmclear")); reader.init(nullptr, &sink); }
 void TearDown() override { reader.clearCache(); sandbox.reset(); }
 std::unique_ptr<Sandbox> sandbox;
 BaseDataMgr bd;
 HotMgr hot;
 RdmSink sink{&bd, &hot};
 WtRdmDtReader reader;
};

TEST_F(Q03RdmClearLifetime, HistoricalTickSurvivesClearAndReloads) {
 const std::string file="his/ticks/SSE/20240108/600000.dsb";
 const auto old=make_tick(20240108,20240108,93000000,1);
 const auto next=make_tick(20240108,20240108,93000000,8);
 ASSERT_TRUE(write_bytes(file,make_block(BT_HIS_Ticks,BLOCK_VERSION_RAW_V2,pack_ticks({old}))));
 auto* saved=reader.readTickSliceByDate("SSE.STK.600000",20240108);
 ASSERT_NE(saved,nullptr); ASSERT_EQ(saved->size(),1u); EXPECT_DOUBLE_EQ(saved->at(0)->price,old.price);
 ASSERT_TRUE(write_bytes(file,make_block(BT_HIS_Ticks,BLOCK_VERSION_RAW_V2,pack_ticks({next}))));
 reader.clearCache();
 EXPECT_DOUBLE_EQ(saved->at(0)->price,old.price);
 auto* fresh=reader.readTickSliceByDate("SSE.STK.600000",20240108);
 ASSERT_NE(fresh,nullptr); ASSERT_EQ(fresh->size(),1u); EXPECT_DOUBLE_EQ(fresh->at(0)->price,next.price);
 fresh->release(); saved->release();
}

TEST_F(Q03RdmClearLifetime, HistoricalQueueSurvivesClearAndReloads) {
 const std::string file="his/queue/SSE/20240108/600000.dsb";
 const auto old=make_ordque(20240108,20240108,93000000,1);
 const auto next=make_ordque(20240108,20240108,93000000,8);
 ASSERT_TRUE(write_bytes(file,make_block(BT_HIS_OrdQueue,BLOCK_VERSION_CMP_V2,pack_items(std::vector<WTSOrdQueStruct>{old}))));
 auto* saved=reader.readOrdQueSliceByRange("SSE.STK.600000",20240108090000000ull,20240108100000000ull);
 ASSERT_NE(saved,nullptr); ASSERT_EQ(saved->size(),1u); EXPECT_DOUBLE_EQ(saved->at(0)->price,old.price);
 ASSERT_TRUE(write_bytes(file,make_block(BT_HIS_OrdQueue,BLOCK_VERSION_CMP_V2,pack_items(std::vector<WTSOrdQueStruct>{next}))));
 reader.clearCache(); EXPECT_DOUBLE_EQ(saved->at(0)->price,old.price);
 auto* fresh=reader.readOrdQueSliceByRange("SSE.STK.600000",20240108090000000ull,20240108100000000ull);
 ASSERT_NE(fresh,nullptr); ASSERT_EQ(fresh->size(),1u); EXPECT_DOUBLE_EQ(fresh->at(0)->price,next.price);
 fresh->release(); saved->release();
}

TEST_F(Q03RdmClearLifetime, HistoricalDetailSurvivesClearAndReloads) {
 const std::string file="his/orders/SSE/20240108/600000.dsb";
 const auto old=make_orddtl(20240108,20240108,93000000,1);
 const auto next=make_orddtl(20240108,20240108,93000000,8);
 ASSERT_TRUE(write_bytes(file,make_block(BT_HIS_OrdDetail,BLOCK_VERSION_CMP_V2,pack_items(std::vector<WTSOrdDtlStruct>{old}))));
 auto* saved=reader.readOrdDtlSliceByRange("SSE.STK.600000",20240108090000000ull,20240108100000000ull);
 ASSERT_NE(saved,nullptr); ASSERT_EQ(saved->size(),1u); EXPECT_DOUBLE_EQ(saved->at(0)->price,old.price);
 ASSERT_TRUE(write_bytes(file,make_block(BT_HIS_OrdDetail,BLOCK_VERSION_CMP_V2,pack_items(std::vector<WTSOrdDtlStruct>{next}))));
 reader.clearCache(); EXPECT_DOUBLE_EQ(saved->at(0)->price,old.price);
 auto* fresh=reader.readOrdDtlSliceByRange("SSE.STK.600000",20240108090000000ull,20240108100000000ull);
 ASSERT_NE(fresh,nullptr); ASSERT_EQ(fresh->size(),1u); EXPECT_DOUBLE_EQ(fresh->at(0)->price,next.price);
 fresh->release(); saved->release();
}

TEST_F(Q03RdmClearLifetime, HistoricalTransSurvivesClearAndReloads) {
 const std::string file="his/trans/SSE/20240108/600000.dsb";
 const auto old=make_trans(20240108,20240108,93000000,1);
 const auto next=make_trans(20240108,20240108,93000000,8);
 ASSERT_TRUE(write_bytes(file,make_block(BT_HIS_Trnsctn,BLOCK_VERSION_CMP_V2,pack_items(std::vector<WTSTransStruct>{old}))));
 auto* saved=reader.readTransSliceByRange("SSE.STK.600000",20240108090000000ull,20240108100000000ull);
 ASSERT_NE(saved,nullptr); ASSERT_EQ(saved->size(),1u); EXPECT_DOUBLE_EQ(saved->at(0)->price,old.price);
 ASSERT_TRUE(write_bytes(file,make_block(BT_HIS_Trnsctn,BLOCK_VERSION_CMP_V2,pack_items(std::vector<WTSTransStruct>{next}))));
 reader.clearCache(); EXPECT_DOUBLE_EQ(saved->at(0)->price,old.price);
 auto* fresh=reader.readTransSliceByRange("SSE.STK.600000",20240108090000000ull,20240108100000000ull);
 ASSERT_NE(fresh,nullptr); ASSERT_EQ(fresh->size(),1u); EXPECT_DOUBLE_EQ(fresh->at(0)->price,next.price);
 fresh->release(); saved->release();
}

TEST_F(Q03RdmClearLifetime, RealtimeTickSurvivesClear) {
 const auto tick=make_tick(20240110,20240110,93000000,1);
 ASSERT_TRUE(write_bytes("rt/ticks/SSE/600000.dmb",make_rt_block<WTSTickStruct>(BT_RT_Ticks,20240110,1,8,{tick})));
 auto* saved=reader.readTickSliceByDate("SSE.STK.600000",20240110);
 ASSERT_NE(saved,nullptr); ASSERT_EQ(saved->size(),1u); EXPECT_DOUBLE_EQ(saved->at(0)->price,tick.price);
 reader.clearCache();
 EXPECT_DOUBLE_EQ(saved->at(0)->price,tick.price);
 saved->release();
}
}
