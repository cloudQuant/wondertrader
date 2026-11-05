from pathlib import Path
repo=Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
s=(repo/'src/TestUnits/coverage/t_TraderMocker/r60_tradermocker_async_test.cpp').read_text()
marker='} // namespace'
assert s.count(marker)==1
s=s.replace(marker,Path('/tmp/wt-i03-r60-queue-audit/probes.inc').read_text()+'\n'+marker)
Path('/tmp/wt-i03-r60-queue-audit/test.cpp').write_text(s)
