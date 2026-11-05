#include "/Users/yunjinqi/Documents/new_projects/wondertrader/src/WTSUtils/lmdb/lmdb.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

void check(int rc) { if (rc) { fprintf(stderr,"LMDB: %s\n",mdb_strerror(rc)); std::abort(); } }
int main(int argc,char** argv) {
 if(argc!=3) return 2; const bool joined=std::string(argv[1])=="joined";
 if(std::filesystem::exists(argv[2])) return 3; std::filesystem::create_directories(argv[2]);
 MDB_env* env=nullptr; MDB_dbi db; MDB_txn* txn=nullptr;
 check(mdb_env_create(&env)); check(mdb_env_set_mapsize(env,64*1024*1024));
 check(mdb_env_open(env,argv[2],std::string(argv[1])=="notls" ? MDB_NOTLS : 0,0600));
 check(mdb_txn_begin(env,nullptr,0,&txn)); check(mdb_dbi_open(txn,nullptr,0,&db));
 std::string data(8192,'x'); MDB_val k{1,const_cast<char*>("k")},v{data.size(),data.data()};
 check(mdb_put(txn,db,&k,&v,0)); check(mdb_txn_commit(txn));
 for(int i=0;i<8;++i) { check(mdb_txn_begin(env,nullptr,0,&txn)); data[0]=static_cast<char>('a'+i);
  check(mdb_put(txn,db,&k,&v,0)); check(mdb_txn_commit(txn)); }
 std::atomic<bool> ready{false}, release{false};
 std::thread reader([&] { MDB_txn* r=nullptr; check(mdb_txn_begin(env,nullptr,MDB_RDONLY,&r));
  MDB_val out{}; check(mdb_get(r,db,&k,&out)); mdb_txn_abort(r); ready.store(true);
  while(!release.load()) std::this_thread::yield(); });
 while(!ready.load()) std::this_thread::yield();
 auto writes=[&] { for(int i=0;i<128;++i) { MDB_txn* w=nullptr; check(mdb_txn_begin(env,nullptr,0,&w));
  data[0]=static_cast<char>(i); check(mdb_put(w,db,&k,&v,0)); check(mdb_txn_commit(w)); } };
 if(joined) { release.store(true); reader.join(); std::thread writer(writes); writer.join(); }
 else { std::thread writer([&]{ while(!release.load()) std::this_thread::yield(); writes(); });
  release.store(true); reader.join(); writer.join(); }
 mdb_env_close(env); std::puts("OK");
}
