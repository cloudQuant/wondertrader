LMDB reader slot TSan control, macOS arm64, 2026-11-02.
Independent single-process single-environment test. control.cpp uses only LMDB,
C++ threads, and atomics; it does not include the migration component.

Build (source first run differs only by notls open flag, which leaves concurrent
and joined modes unchanged):
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=thread /tmp/wt-i03-tsan-slot-control-20261102/control.cpp /tmp/wt-i03-capability-final-20261102/tsan-raw-mdb.o /tmp/wt-i03-capability-final-20261102/tsan-raw-midl.o -o /tmp/wt-i03-tsan-slot-control-20261102/control-notls

TSAN_OPTIONS=halt_on_error=1:exitcode=66 control-notls MODE NEW_DIRECTORY
concurrent: exit 134; mdb_find_oldest mdb.c:2127 read and
mdb_env_reader_dest mdb.c:4517 write to mr_pid; see concurrent.log.
joined: exit 0, joined.log. Reader is joined before writer starts.
notls: exit 0, notls2.log. MDB_NOTLS releases the reader slot in transaction
abort, which occurs before a flag that synchronizes with the writer; this does
not reproduce the asynchronous TLS destructor. These negative controls do not
prove other concurrent operations or platforms race-free.

Repository LMDB explicitly documents lock-free writer reader-table scans and
stores mr_pid as volatile; mdb_txn_end sets mr_txnid=-1 before the thread's TLS
cleanup sets mr_pid=0. In this controlled sequence the reported cleanup follows
read abort; no data corruption was observed or excluded. Volatile is not a C
synchronization primitive, and neither the comment about atomic slot cleanup nor
absence of a report under a different schedule proves portable correctness. The
report is a real conflicting-access observation, not enough to decide its
runtime consequence or whether it can be suppressed safely. Raw and semaphore-
annotated migration test logs both report this slot conflict.
