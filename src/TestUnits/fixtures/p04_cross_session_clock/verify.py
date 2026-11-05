#!/Users/yunjinqi/opt/anaconda3/bin/python
import os,subprocess,sys
from pathlib import Path
binary=Path(sys.argv[1]).resolve(); out=Path(sys.argv[2]).resolve()
if out.exists(): raise SystemExit('output directory must not already exist')
out.mkdir(parents=True)
green=['R43ReplayerTickCsvTest.'+n for n in ('sessionBeginClockUsesRangeStartThenNextNaturalSessionOpen','overnightTickRangeLoadsByTradingDateAndFiltersByActionDate','nightSessionBeginClockUsesNaturalOpenAcrossTradingDates','mainBarsUseReplayableHftSessionForMixedNaturalDate','replayableHftEventsKeepLaterSessionBoundary','missingInRangeHftEventsKeepLaterSessionBoundary','mainBarWithoutInRangeHftEventsKeepsLaterSessionBoundary','mainBarAndNightSessionBucketsReplayInNaturalTimeOrder')]
green+=['P07TickLoaderTest.'+n for n in ('emptyReplayBucketsStayBoundedAcrossLongNoDataRange','overnightTickBucketAcrossWeekendUsesTradingCalendarAndResets','continuous24HourReplayReadsFullSaturdayNaturalDateBucket','continuous24HourPositiveOffsetUsesNaturalDateCut','continuous24HourNegativeOffsetUsesNaturalDateCut','asynchronousOvernightBucketDrainsAfterMainBarAcrossWeekend','sameTimestampOrdersRoundsAcrossCodesAndTradingDateBuckets','tickInSessionGapIsSkippedUntilNextSession','emptyLevel2StreamsDoNotBlockTickReplay','pureTickContinuousAndFutureDispatchOnlyWithNoDataDay')]
cases=[('green',':'.join(green),0),('known-red-slice','P07TickLoaderTest.pureTickContinuousAndFutureNoDataDayAndFinalMillisecond',1),('known-red-negative-offset','P07TickLoaderTest.pureTickMixedSessionsNoDataDayAndFinalMillisecond',1)]
for name, filt, expected in cases:
 env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(out/(name+'-%p.profraw'))
 with (out/(name+'.log')).open('w') as log:
  result=subprocess.run([str(binary),'--gtest_filter='+filt],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=60)
 print(name,'exit',result.returncode,'expected',expected,flush=True)
 if result.returncode!=expected: raise SystemExit(1)
