from datetime import date,timedelta
import calendar,random,subprocess,os
rng=random.Random(20261102)
inputs=[]; expected=[]
for i in range(10000):
 y=rng.randint(1,9999);m=rng.randint(1,12);d=rng.randint(1,calendar.monthrange(y,m)[1]);minute=rng.randint(0,1440);t=(minute//60)*100+minute%60;o=rng.randint(-2147483648,2147483647)
 shift,rem=divmod(minute+o,1440)
 try: end=date(y,m,d)+timedelta(days=shift); success=1; result=end.year*10000+end.month*100+end.day
 except OverflowError: success=0;result=0xabcdef
 left=rem//60*100+rem%60;right=left if rem else 2400
 inputs.append(f'{o} {y*10000+m*100+d} {t}\n');expected.append((success,result,left,right))
p=subprocess.run(['/tmp/wt-l03-multiday-20261102/current_vectors'],input=''.join(inputs),capture_output=True,text=True,timeout=30,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
if p.returncode:print('PROCESS ERROR',p.returncode,p.stderr);raise SystemExit(1)
rows=[tuple(map(int,x.split())) for x in p.stdout.splitlines()]
if len(rows)!=len(expected):print('COUNT ERROR',len(rows));raise SystemExit(1)
for i,(actual,want) in enumerate(zip(rows,expected)):
 if actual!=want:print('MISMATCH',i,inputs[i].strip(),actual,want);raise SystemExit(1)
print('10000 deterministic randomized date/offset vectors matched; ASan/UBSan exit 0')
