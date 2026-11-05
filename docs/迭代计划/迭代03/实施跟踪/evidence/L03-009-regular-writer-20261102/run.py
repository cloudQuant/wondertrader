import json, os, pathlib, subprocess, sys
binary_dir = pathlib.Path(sys.argv[1]); output_dir = pathlib.Path(sys.argv[2]); output_dir.mkdir(parents=True, exist_ok=True)
names = ('InvalidDateRejectedBeforeSynchronousEffectsAndCanRetry','InvalidDateRejectedBeforeAsyncAdmission','MissingAndMalformedDatesAndMissingSessionRejected','AcceptedQueuedTickRecheckedBeforeEffects','SynchronousSinkReentryCannotPublishInvalidDate','MissingContractKeepsAcceptedNoOpBehavior')
rows=[]
for variant in ('red','green'):
 for name in names:
  argv=[str(binary_dir / f'{variant}-test'), f'--gtest_filter=L03WriterSessionDate.{name}', f'--gtest_output=xml:{(output_dir / f"{variant}-{name}.xml").resolve()}']
  env=dict(os.environ,TZ='UTC',ASAN_OPTIONS='abort_on_error=1:detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',LLVM_PROFILE_FILE=f'/tmp/wt-l03-fresh-{variant}-%p.profraw')
  try:
   p=subprocess.run(argv,env=env,text=True,capture_output=True,timeout=30); code=p.returncode; output=p.stdout+p.stderr
  except subprocess.TimeoutExpired as exc:
   code='TIMEOUT'; output=str(exc)
  (output_dir/f'{variant}-{name}.log').write_text(output); rows.append(dict(variant=variant,name=name,argv=argv,exit=code)); print(variant,name,code,flush=True)
(output_dir/'results.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n')
