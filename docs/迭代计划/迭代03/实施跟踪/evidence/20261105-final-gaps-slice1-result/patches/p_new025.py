# I03-NEW-025: make literals/constants match the signedness of the compared value.
# Assertion semantics unchanged (all compared constants are non-negative).
import os, sys, shutil
ROOT='/Users/yunjinqi/Documents/new_projects/wondertrader/src/TestUnits/coverage/'
BEFORE=sys.argv[1] if len(sys.argv)>1 else None
S=[
('l01_share_wtsutils/l01_codehelper_test.cpp',559,'MAX_INSTRUMENT_LENGTH - 1);','static_cast<std::size_t>(MAX_INSTRUMENT_LENGTH - 1));'),
('l01_share_wtsutils/l01_codehelper_test.cpp',572,'MAX_INSTRUMENT_LENGTH);','static_cast<std::size_t>(MAX_INSTRUMENT_LENGTH));'),
('n01_wtcore_tradepaths/r71_hft_ctx_test.cpp',319,'bars->size(), 3u)','bars->size(), 3)'),
('n02_wtcore_engines/r73_cta_ctx_test.cpp',187,'bars->size(), 3u)','bars->size(), 3)'),
('p03_btcore_mockers2/r190_his_residual_test.cpp',172,'(5u, catchup->size())','(5, catchup->size())'),
('p03_btcore_mockers2/r200_hisdata_test.cpp',294,'(5u, catchup->size())','(5, catchup->size())'),
('p03_btcore_mockers2/r72_cta_sel_gap_test.cpp',236,'bars->size(), 1u)','bars->size(), 1)'),
('p03_btcore_mockers2/r72_replayer_block_edge_test.cpp',119,'s->size(), 1u)','s->size(), 1)'),
('p03_btcore_mockers2/r72_replayer_block_edge_test.cpp',176,'(2u, s->size())','(2, s->size())'),
('p04_btcore_replayer/p04_replayer_csv_replay_test.cpp',629,'(1u, main->size())','(1, main->size())'),
('p04_btcore_replayer/r43_replayer_init_basefiles_test.cpp',228,'(1u, slice->size())','(1, slice->size())'),
('p05_datastorage2/r100_reader_ad_test.cpp',90,'slice->size(), 1u)','slice->size(), 1)'),
('p05_datastorage2/r112_reader_ad_day_test.cpp',57,'dk->size(), 1u)','dk->size(), 1)'),
('p05_datastorage2/r112_reader_ad_kline_test.cpp',58,'s1->size(), 2u)','s1->size(), 2)'),
('p05_datastorage2/r122_reader_ad_branches_test.cpp',64,'s1->size(), 3u)','s1->size(), 3)'),
('p05_datastorage2/r122_reader_ad_branches_test.cpp',151,'(3u, first->size())','(3, first->size())'),
('p05_datastorage2/r142_reader_ad_residual_test.cpp',67,'s1->size(), 3u)','s1->size(), 3)'),
('p05_datastorage2/r4_reader_ad_slices_test.cpp',2052,'slice->size(), 1u)','slice->size(), 1)'),
('p08_btcore_mockers3/r151_replayer_paths_test.cpp',76,'slice->size(), 0u)','slice->size(), 0)'),
('p09_storage_readers/p09_rdm_window_range_test.cpp',429,'range->size(), 3u)','range->size(), 3)'),
('q02_btcore3/r103_replayer_tick_csv_test.cpp',59,'(1, slice->size())','(1u, slice->size())'),
('q02_btcore3/r80_replayer_slice_unsub_test.cpp',180,'(1u, sink.slice->size())','(1, sink.slice->size())'),
('q02_btcore3/r80_replayer_task_bars_test.cpp',82,'(1u, sink.slice->size())','(1, sink.slice->size())'),
('q03_storage3/r100_reader_crossday_test.cpp',238,'qfq->size(), 2u)','qfq->size(), 2)'),
('q03_storage3/r142_rdm_hft_his_test.cpp',773,'s->size(), 1u)','s->size(), 1)'),
('q03_storage3/r182_rdm_rt_slices_test.cpp',60,'slice->size(), 2u)','slice->size(), 2)'),
('q03_storage3/r182_reader_integrated_edges_test.cpp',75,'slice->size(), 0u)','slice->size(), 0)'),
('q03_storage3/r193_storage_reader_residual_test.cpp',80,'slice->size(), 1u)','slice->size(), 1)'),
('q03_storage3/r52_reader_hot_adj_test.cpp',126,'slice->size(), 2u)','slice->size(), 2)'),
('r19_mq/r19_shareblock_cmd_test.cpp',299,'(MAX_KEY_CNT, block->_capacity)','(static_cast<uint32_t>(MAX_KEY_CNT), block->_capacity)'),
('r22_dtporter/r22_ext_porter_test.cpp',675,'(0, tick.action_date)','(0u, tick.action_date)'),
('r23_storage4/r23_rdm_count_slice_test.cpp',352,'slice->size(), 2u)','slice->size(), 2)'),
('r23_storage4/r52_rdm_hot_adj_test.cpp',379,'exactSingle->size(), 1u)','exactSingle->size(), 1)'),
('t_ParserFemas/femas_parser_callbacks_test.cpp',405,'(0u, strcmp(','(0, strcmp('),
('t_ParserXTP/cov_parserxtp_spi_test.cpp',187,'(1, spi.bdMgrQueryCalls)','(1u, spi.bdMgrQueryCalls)'),
('t_TraderCTPMini/r61_traderctpmini_api_test.cpp',1044,'(1u, api.action.FrontID)','(1, api.action.FrontID)'),
('t_TraderFemas/femas_trader_test.cpp',683,'TimeUtils::makeTime(20240105, 93015000))','static_cast<uint64_t>(TimeUtils::makeTime(20240105, 93015000)))'),
('t_TraderYD/t5_yd_api_test.cpp',434,'(0u, second.compare(','(0, second.compare('),
]
files={}
for f,n,o,nw in S: files.setdefault(f,[]).append((n,o,nw))
for f,edits in files.items():
    p=ROOT+f; raw=open(p,'rb').read()
    lines=raw.split(b'\n')
    for n,o,nw in edits:
        L=lines[n-1]; ob=o.encode(); nb=nw.encode()
        assert L.count(ob)==1,(f,n,L)
        lines[n-1]=L.replace(ob,nb)
    out=b'\n'.join(lines)
    assert len(out.split(b'\n'))==len(raw.split(b'\n')) and out.count(b'\r\n')==raw.count(b'\r\n')
    if BEFORE:
        dst=os.path.join(BEFORE,('src/TestUnits/coverage/'+f).replace('/','__')); shutil.copy2(p,dst)
    tmp=p+'.s1tmp'; open(tmp,'wb').write(out); shutil.copymode(p,tmp); os.replace(tmp,p)
print('edited',len(files),'files',len(S),'sites')
