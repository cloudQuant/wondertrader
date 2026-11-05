// Local-only proof that LMDB transaction IDs cannot replace prefix audits.
#define main prior_fixture_main
#include "../lmdb_bar_migration/migration_test.cpp"
#undef main
#include <fstream>

namespace {
void evidence(Bindings& binding) {
 for(const auto& row:rows) {
  auto k=key(row); auto v=value(row); MDB_val a{k.size(),k.data()}, b{v.size(),v.data()};
  require(binding.putEvidence(a,b,Evidence{row.ex,row.code,row.time,true}),"evidence load");
 }
}
Result safe(const Bindings& b,std::size_t limit=1) {
 return stageBatch(b,LegacySourceHost::LittleEndian,LegacyValueLayout::Aligned64,limit);
}
std::size_t txnId(MDB_env* e) { MDB_envinfo info{}; mcheck(mdb_env_info(e,&info)); return info.me_last_txnid; }
// Modify only this fixture's closed test file; source and destination are
// ordinary isolated files. Skipping evidence records isolates staging values.
std::size_t rawChange(const fs::path& file, const Row& row, bool skipEvidence) {
 std::fstream stream(file,std::ios::in|std::ios::out|std::ios::binary);
 require(bool(stream),"open data file"); stream.seekg(0,std::ios::end);
 const auto length=stream.tellg(); require(length>0,"data length"); stream.seekg(0);
 std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
 stream.read(reinterpret_cast<char*>(bytes.data()),length); require(bool(stream),"read data file");
 auto v=value(row); std::size_t changed=0;
 for(std::size_t i=0;i+v.size()<=bytes.size();++i) {
  if(std::memcmp(bytes.data()+i,v.data(),v.size())!=0) continue;
  bool evidencePrefix=i>=12 && bytes[i-12]==1 && bytes[i-11]==v.size() &&
    bytes[i-10]==std::strlen(row.ex) && bytes[i-9]==std::strlen(row.code);
  if(evidencePrefix) for(unsigned j=0;j<8;++j)
    if(bytes[i-8+j]!=static_cast<unsigned char>(row.time >> (56-j*8))) evidencePrefix=false;
  if(skipEvidence && evidencePrefix) continue;
  bytes[i+40]^=1; ++changed; i+=v.size()-1;
 }
 require(changed>0,"no matching data to modify");
 stream.seekp(0); stream.write(reinterpret_cast<const char*>(bytes.data()),length);
 stream.flush(); require(bool(stream),"write data file");
 return changed;
}
}

int main(int argc,char** argv) {
 try {
  require(argc==3,"usage mode new-directory"); const std::string mode=argv[1]; const fs::path root=argv[2];
  if(mode=="raw-worker-source" || mode=="raw-worker-target") {
   const bool target=mode=="raw-worker-target";
   rawChange(root/(target?"dest":"source")/"data.mdb",rows[3],target); return 0;
  }
  require(!fs::exists(root),"directory already exists"); fs::create_directories(root);
  const auto source=root/"source",dest=root/"dest"; seed(source);
  if(mode=="multiple-bindings") {
   // Deliberately violate the one-binding-per-destination contract. Sequential
   // initialization avoids concurrent LMDB DBI opens; this is a negative control.
   Env s(source,true),d(dest,false); Bindings a,b; bind(s.p,d.p,a); evidence(a);
   require(safe(a).code==ResultCode::Incomplete,"first batch");
   bind(s.p,d.p,b); auto k=key(rows[3]); MDB_val mk{k.size(),k.data()};
   auto before=txnId(d.p); require(b.revokeEvidence(mk),"second binding could not revoke");
   auto after=txnId(d.p); require(after>before,"txn id did not advance");
   require(safe(a).code==ResultCode::MissingEvidence,"first binding missed revocation");
   require(manifestState(d.p)=='I',"unexpected completion");
   std::cout<<mode<<" second_binding_mutation_allowed=1 target_txnid_before="<<before
            <<" after="<<after<<" audit=MissingEvidence\n"; return 0;
  }
  std::size_t sourceId=0,targetId=0;
  {
   Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); evidence(b);
   require(safe(b).code==ResultCode::Incomplete,"first batch");
   sourceId=txnId(s.p); targetId=txnId(d.p);
   if(mode=="source-live" || mode=="target-live") {
    // A separate non-cooperating process modifies this fixture's physical file
    // after the transaction ends, while the original binding stays alive.
    pid_t pid=fork(); require(pid>=0,"fork");
    if(pid==0) {
     const char* childMode=mode=="source-live"?"raw-worker-source":"raw-worker-target";
     execl(argv[0],argv[0],childMode,root.c_str(),static_cast<char*>(nullptr)); _exit(127);
    }
    int childStatus=0;
    require(waitpid(pid,&childStatus,0)==pid && WIFEXITED(childStatus) && WEXITSTATUS(childStatus)==0,"raw writer child");
    require(txnId(s.p)==sourceId && txnId(d.p)==targetId,"raw edit changed txn id");
    auto r=safe(b);
    require(r.code==(mode=="source-live"?ResultCode::MissingEvidence:ResultCode::Conflict),"live edit not detected");
    require(manifestState(d.p)=='I',"unexpected completion");
    std::cout<<mode<<" unchanged_source_txnid="<<sourceId<<" unchanged_target_txnid="<<targetId
             <<" audit_code="<<static_cast<int>(r.code)<<"\n"; return 0;
   }
  }
  // rows[3] is lexicographically first and thus included in the committed prefix.
  if(mode=="source-raw") rawChange(source/"data.mdb",rows[3],false);
  else if(mode=="target-raw") rawChange(dest/"data.mdb",rows[3],true);
  else throw std::runtime_error("unknown mode");
  Env s(source,true),d(dest,false);
  require(txnId(s.p)==sourceId && txnId(d.p)==targetId,"raw edit changed txn id");
  Bindings b; bind(s.p,d.p,b); auto r=safe(b);
  if(mode=="source-raw") require(r.code==ResultCode::MissingEvidence,"source edit not detected");
  else require(r.code==ResultCode::Conflict,"target edit not detected");
  require(manifestState(d.p)=='I',"unexpected completion");
  std::cout<<mode<<" unchanged_source_txnid="<<sourceId<<" unchanged_target_txnid="<<targetId
           <<" audit_code="<<static_cast<int>(r.code)<<"\n";
  return 0;
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
