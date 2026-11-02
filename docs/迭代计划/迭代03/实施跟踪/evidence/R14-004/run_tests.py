import os,subprocess,json,hashlib,xml.etree.ElementTree as ET
from pathlib import Path
root=Path('/tmp/wt-r14-resume'); out=root/'runs';out.mkdir(exist_ok=True)
binroot=root/'build/build_x64/Debug/bin'
quote=binroot/'TestUnits/CoverageTests_m_QuoteFactory'; runner=binroot/'TestUnits/CoverageTests_m_WtBtRunner'
qnames=['r14_quote_entry.short_help_returns_zero_before_any_logging_init','r14_quote_entry.long_help_returns_zero','r14_quote_entry.help_wins_over_config_and_logcfg_words','r14_quote_entry.missing_config_path_returns_nonzero_via_explicit_c','r14_quote_entry.both_c_and_l_given_still_returns_nonzero_when_files_absent','r14_quote_entry.missing_default_config_returns_nonzero_in_empty_directory','r14_quote_initialize.valid_basefiles_object_remains_accepted','r14_quote_cli.valid_config_with_real_writer_returns_zero_after_signal']
rnames=['r14_wtbtrunner_cli.short_help_exits_zero_in_child','r14_wtbtrunner_cli.help_precedes_explicit_config_check','r14_wtbtrunner_cli.explicit_missing_config_exits_nonzero_with_diagnostic','r14_wtbtrunner_cli.missing_default_config_exits_nonzero_in_empty_directory','r14_wtbtrunner_cli.selTaskDefaultsAllowRunWhenTemplatesAreOmitted']
results=[]
for binary,names in [(quote,qnames),(runner,rnames)]:
 for name in names:
  run=out/name;run.mkdir(exist_ok=True);xml=run/'result.xml';env=dict(os.environ);env['TMPDIR']=str(run)+'/';env['LLVM_PROFILE_FILE']=str(run/'profile-%p.profraw');env['WT_R14_QF_PRODUCT_BIN']=str(binroot/'QuoteFactory/QuoteFactory')
  cmd=[str(binary),'--gtest_filter='+name,'--gtest_output=xml:'+str(xml)]
  try:
   p=subprocess.run(cmd,cwd=run,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=35)
   rc=p.returncode;output=p.stdout
  except subprocess.TimeoutExpired as e:
   rc='timeout';output=e.stdout or b''
  (run/'output.log').write_bytes(output)
  data={'test':name,'argv':cmd,'cwd':str(run),'exit':rc}
  if xml.exists():
   x=ET.parse(xml).getroot();data['xml']={k:x.get(k) for k in ['tests','failures','disabled','errors']}
  results.append(data)
  (root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
  print(name,rc,data.get('xml'),flush=True)
