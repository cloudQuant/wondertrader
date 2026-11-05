#include "test_support.hpp"
#include <cstdio>
#include <cstring>
unsigned ExpSelContext::made=0;
WtRtRunner& getRunner() {static WtRtRunner runner; return runner;}
int main(int argc,char** argv) {
 if(argc!=2)return 2;
 if(std::strcmp(argv[1],"valid")==0) {
  const char* tokens[]={"d","D","w","m","y","min","MIN"};
  const TaskPeriodType types[]={TPT_Daily,TPT_Daily,TPT_Weekly,TPT_Monthly,TPT_Yearly,TPT_Minute,TPT_Minute};
  for(unsigned i=0;i<7;i++) {
   auto h=create_sel_context("s",20250110,93000,tokens[i],"CHINA","TRADING",0);
   if(h==0 || getRunner()._sel_engine.types.size()!=i+1 || getRunner()._sel_engine.types.back()!=types[i]) return 1;
  }
  return 0;
 }
 if(std::strcmp(argv[1],"invalid")==0) {
  const char* tokens[]={"", "unknown", "daily", "M2"};
  for(auto p:tokens) {
   auto h=create_sel_context("s",20250110,93000,p,"CHINA","TRADING",0);
   if(h!=0 || ExpSelContext::made!=0 || !getRunner()._sel_engine.types.empty())return 1;
  }
  return 0;
 }
 if(std::strcmp(argv[1],"null")==0) {
  auto h=create_sel_context("s",20250110,93000,nullptr,"CHINA","TRADING",0);
  return h!=0 || ExpSelContext::made!=0 || !getRunner()._sel_engine.types.empty();
 }
 if(std::strcmp(argv[1],"documented")==0) {
  for(auto p:{"d1","w1","m1"}) { auto h=create_sel_context("s",20250110,93000,p,"CHINA","TRADING",0); if(h==0 || getRunner()._sel_engine.types.back()==TPT_None)return 1; }
  return 0;
 }
 return 2;
}
