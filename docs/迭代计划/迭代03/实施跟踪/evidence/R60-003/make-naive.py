from pathlib import Path
s=Path('/tmp/wt-i03-r60-lifecycle/baseline/TraderMocker.cpp').read_text()
a='void TraderMocker::connect()\n{\n\treconn_udp();'
b='void TraderMocker::connect()\n{\n\tif (_io_service.stopped()) _io_service.restart();\n\t_terminated.store(false, std::memory_order_release);\n\treconn_udp();'
assert s.count(a)==1
Path('/tmp/wt-i03-r60-lifecycle/naive.cpp').write_text(s.replace(a,b))
