# K05 follow-up: a destroyed adapter leaves no in-flight contribution in a shared gate.
ROOT='/Users/yunjinqi/Documents/new_projects/wondertrader/src/'
p=ROOT+'WtCore/TraderAdapter.cpp'; raw=open(p,'rb').read(); bom=raw.startswith(b'\xef\xbb\xbf')
s=raw.decode('utf-8-sig').replace('\r\n','\n')
old='''	if (_cfg)
		_cfg->release();

	if (_stat_map)
		_stat_map->release();
}

thread_local const TraderAdapter::SpiIngress* TraderAdapter::s_spi_ingress = NULL;
'''
new='''	if (_cfg)
		_cfg->release();

	if (_stat_map)
		_stat_map->release();

	// K05 (slice1 final gaps): release() above may still have changed this
	// adapter's undone quantity after it left the shared account gate.
	if (_account_gate)
		_account_gate->clearInflight(this);
}

thread_local const TraderAdapter::SpiIngress* TraderAdapter::s_spi_ingress = NULL;
'''
assert s.count(old)==1; s=s.replace(old,new)
open(p,'wb').write((b'\xef\xbb\xbf' if bom else b'')+s.replace('\n','\r\n').encode('utf-8'))
print('applied trader2')
