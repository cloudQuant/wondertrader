import sys
root=sys.argv[1]
p=root+'/src/WtBtPorter/WtBtRunner.cpp'
s=open(p).read()
old='''	if (_exec_mocker)
	{
		if (!quiesceAndDestroyExecMocker(error))
			return false;
	}

	if (_cfg)
	{
		_cfg->release();
		_cfg = NULL;
	}
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		_released = true;
	}
	return true;
}'''
new='''	if (_exec_mocker)
	{
		if (!quiesceAndDestroyExecMocker(error))
			return false;
	}

	// R20_wtbtcore#3 / 卡9：回放已停止且线程已 join，此时排空通知器在途发布并恰好一次
	// 销毁 MQ server；之后 Runner/EventNotifier 析构不会重复销毁，模块按 9a=A 常驻。
	_notifier.release();

	if (_cfg)
	{
		_cfg->release();
		_cfg = NULL;
	}
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		_released = true;
	}
	return true;
}'''
assert s.count(old)==1; s=s.replace(old,new,1)
old2='''		WTSLogger::error("Preparing historical data replayer failed; backtest will not run");'''
new2='''		const WtReplayStatusV1 status = _replayer.replay_status();
		WTSLogger::error("Preparing historical data replayer failed; backtest will not run: replay error {}: {}",
			status.error_code, status.message[0] != '\\0' ? status.message : "no message");'''
assert s.count(old2)==1; s=s.replace(old2,new2,1)
open(p,'w').write(s)

p=root+'/src/WtBtRunner/WtBtRunner.cpp'
s=open(p).read()
old3='''	if (!replayer->prepare())
	{
		WTSLogger::error("Preparing historical data replayer failed");'''
new3='''	if (!replayer->prepare())
	{
		// I03-NEW-019：输出回放器记录的错误码与消息，便于定位 prepare 失败原因
		const WtReplayStatusV1 status = replayer->replay_status();
		WTSLogger::error("Preparing historical data replayer failed: replay error {}: {}",
			status.error_code, status.message[0] != '\\0' ? status.message : "no message");'''
assert s.count(old3)==1; s=s.replace(old3,new3,1)
open(p,'w').write(s)
print("runner patched")
