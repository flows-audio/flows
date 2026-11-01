#include "host/app/FlowsProcessor.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <cstdlib>
#include <sys/stat.h>
using namespace juce;using namespace flows::app;
void check(bool x,const char* m){if(!x)throw std::runtime_error(m);}
int modeBits(const File& file){struct stat info{};check(::stat(file.getFullPathName().toRawUTF8(),&info)==0,"Cannot stat Live bridge path");return info.st_mode&0777;}
int main(int argc,char**argv){
 ScopedJuceInitialiser_GUI init;
 const auto temp=File::getSpecialLocation(File::tempDirectory).getChildFile("flows-companion-"+Uuid().toString());temp.createDirectory();
 auto workspace=temp.getChildFile("Workspace"),ipc=temp.getChildFile("Bridge");workspace.createDirectory();
 ::setenv("FLOWS_WORKSPACE",workspace.getFullPathName().toRawUTF8(),1);::setenv("FLOWS_AI_CONFIG_DIR",temp.getChildFile("Models").getFullPathName().toRawUTF8(),1);::setenv("FLOWS_LIVE_SYNTH_DIR",ipc.getFullPathName().toRawUTF8(),1);
 const String code="import standard as std\nfrom lib.osc import ClassicSine\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch, level: -12db)\n    return raw * key.gate * key.velocity\n";
 try{{FlowsProcessor p(false);p.prepareToPlay(48000,128);p.request("document.new",object({{"name","Live test"}}));auto d=get(p.snapshot(),"document");p.request("document.apply",object({{"documentId",str(d,"id")},{"baseRevision",get(d,"revision")},{"text",code}}));
 auto view=p.companionRequest("read",{});check(str(view,"source")==code,"Read must return unsaved source");
 const auto candidate=code.replace("-12db","-18db");auto proposed=p.companionRequest("propose",object({{"documentId",get(view,"documentId")},{"baseRevision",get(view,"revision")},{"source",candidate}}));check(static_cast<bool>(get(proposed,"ok")),"Proposal must compile/render");check(str(get(proposed,"comparison"),"status")=="compared","Preview comparison did not run");check(str(p.companionRequest("read",{}),"source")==code,"Preview must not mutate source");check(str(get(proposed,"render"),"wav").length()>1000,"Preview must contain real WAV audio");
 auto applied=p.companionRequest("apply",object({{"proposalId",get(proposed,"proposalId")}}));check(static_cast<bool>(get(applied,"applied")),"Apply failed");check(str(p.companionRequest("read",{}),"source")==candidate,"Apply did not update source");
 auto restored=p.companionRequest("restore",object({{"receipt",get(applied,"receipt")}}));check(static_cast<bool>(get(restored,"restored")),"Restore failed");check(str(p.companionRequest("read",{}),"source")==code,"Restore lost original source");
 bool stale=false;try{p.companionRequest("apply",object({{"proposalId",get(proposed,"proposalId")}}));}catch(...){stale=true;}check(stale,"Duplicate apply must fail");
 auto now=p.companionRequest("read",{});auto bad=p.companionRequest("propose",object({{"documentId",get(now,"documentId")},{"baseRevision",get(now,"revision")},{"source","not valid source"}}));check(!static_cast<bool>(get(bad,"ok"))&&str(bad,"proposalId").isEmpty(),"Invalid source must not become an applicable proposal");
 auto second=p.companionRequest("propose",object({{"documentId",get(now,"documentId")},{"baseRevision",get(now,"revision")},{"source",candidate}}));d=get(p.snapshot(),"document");p.request("document.apply",object({{"documentId",str(d,"id")},{"baseRevision",get(d,"revision")},{"text",code+"\n# user edit\n"}}));stale=false;try{p.companionRequest("apply",object({{"proposalId",get(second,"proposalId")}}));}catch(...){stale=true;}check(stale,"Apply must protect intervening edits");check(str(p.companionRequest("read",{}),"source").contains("# user edit"),"Failed apply changed the user edit");
 auto edited=str(p.companionRequest("read",{}), "source");auto held=p.companionRequest("read",{});auto third=p.companionRequest("propose",object({{"documentId",get(held,"documentId")},{"baseRevision",get(held,"revision")},{"source",candidate}}));check(static_cast<bool>(get(third,"ok")),"Proposal after a user edit must compile");auto appliedEdit=p.companionRequest("apply",object({{"proposalId",get(third,"proposalId")}}));p.request("document.apply",object({{"documentId",str(d,"id")},{"baseRevision",get(get(p.snapshot(),"document"),"revision")},{"text",edited+"\n# after apply\n"}}));stale=false;try{p.companionRequest("restore",object({{"receipt",get(appliedEdit,"receipt")}}));}catch(...){stale=true;}check(stale,"Restore must keep edits made after apply");check(str(p.companionRequest("read",{}),"source").contains("# after apply"),"Restore wiped an edit made after apply");
 auto repairBase=str(p.companionRequest("read",{}),"source");auto repairDoc=get(p.snapshot(),"document");p.request("document.apply",object({{"documentId",str(repairDoc,"id")},{"baseRevision",get(repairDoc,"revision")},{"text","not valid source"}}));
 auto broken=p.companionRequest("read",{});auto repair=p.companionRequest("propose",object({{"documentId",get(broken,"documentId")},{"baseRevision",get(broken,"revision")},{"source",candidate}}));
 check(static_cast<bool>(get(repair,"ok"))&&str(repair,"proposalId").isNotEmpty(),"A valid candidate must stay proposable when the current instrument cannot render");
 check(str(get(repair,"comparison"),"status")!="compared"&&str(get(repair,"comparison"),"error").isNotEmpty(),"Baseline render failure must stay visible");
 check(str(p.companionRequest("read",{}),"source")=="not valid source","Repair proposal changed the broken source");
 repairDoc=get(p.snapshot(),"document");p.request("document.apply",object({{"documentId",str(repairDoc,"id")},{"baseRevision",get(repairDoc,"revision")},{"text",repairBase}}));
 auto quotaBase=str(p.companionRequest("read",{}),"source");
 for(int i=0;i<64;++i){
  auto held=p.companionRequest("read",{});
  auto opened=p.companionRequest("propose",object({{"documentId",get(held,"documentId")},{"baseRevision",get(held,"revision")},{"source",candidate}}));
  check(static_cast<bool>(get(opened,"ok")),"Open proposal was rejected before the cap");
 }
 auto capped=p.companionRequest("read",{});bool full=false;try{p.companionRequest("propose",object({{"documentId",get(capped,"documentId")},{"baseRevision",get(capped,"revision")},{"source",candidate}}));}catch(const std::exception& error){full=String(error.what()).contains("open instrument proposals");}
 check(full,"A 65th proposal must be rejected while 64 can still be applied");
 auto quotaDoc=get(p.snapshot(),"document");p.request("document.apply",object({{"documentId",str(quotaDoc,"id")},{"baseRevision",get(quotaDoc,"revision")},{"text",quotaBase+"\n# quota\n"}}));
 auto freed=p.companionRequest("read",{});auto afterEdit=p.companionRequest("propose",object({{"documentId",get(freed,"documentId")},{"baseRevision",get(freed,"revision")},{"source",candidate}}));
 check(static_cast<bool>(get(afterEdit,"ok")),"Changing the instrument did not free proposals it invalidated");
 for(int i=0;i<64;++i){
  auto held=p.companionRequest("read",{});
  auto opened=p.companionRequest("propose",object({{"documentId",get(held,"documentId")},{"baseRevision",get(held,"revision")},{"source",candidate}}));
  check(static_cast<bool>(get(opened,"ok")),"A restored proposal was not reclaimed");
  auto appliedTurn=p.companionRequest("apply",object({{"proposalId",get(opened,"proposalId")}}));
  check(static_cast<bool>(get(p.companionRequest("restore",object({{"receipt",get(appliedTurn,"receipt")}})),"restored")),"Restore during proposal recycling failed");
 }
 quotaDoc=get(p.snapshot(),"document");p.request("document.apply",object({{"documentId",str(quotaDoc,"id")},{"baseRevision",get(quotaDoc,"revision")},{"text",quotaBase}}));
 File folder;for(int i=0;i<100;++i){auto dirs=ipc.findChildFiles(File::findDirectories,false);if(dirs.size()){folder=dirs[0];if(folder.getChildFile("instance.json").existsAsFile())break;}std::this_thread::sleep_for(std::chrono::milliseconds(20));}
 check(folder.getChildFile("instance.json").existsAsFile(),"Editor-independent discovery failed");auto id=Uuid().toString();auto request=folder.getChildFile("requests").getChildFile(id+".json");request.replaceWithText(JSON::toString(object({{"id",id},{"method","read"},{"params",object({})}})));
 auto response=folder.getChildFile("responses").getChildFile(id+".json");for(int i=0;i<100&&!response.existsAsFile();++i)std::this_thread::sleep_for(std::chrono::milliseconds(20));check(response.existsAsFile(),"IPC read timed out");auto result=JSON::parse(response.loadFileAsString());check(str(result,"id")==id&&str(get(result,"result"),"source").contains("user edit"),"IPC read mismatch");
 check(modeBits(ipc)==0700&&modeBits(folder)==0700&&modeBits(folder.getChildFile("requests"))==0700&&modeBits(folder.getChildFile("responses"))==0700,"Live bridge directory is not private");
 check(modeBits(folder.getChildFile("instance.json"))==0600&&modeBits(response)==0600,"Live bridge file is not private");
 auto waitResponse=[&](const String& requestId){auto file=folder.getChildFile("responses").getChildFile(requestId+".json");for(int i=0;i<100&&!file.existsAsFile();++i)std::this_thread::sleep_for(std::chrono::milliseconds(20));check(file.existsAsFile(),"IPC response timed out");return parseJSONBounded(file.loadFileAsString(),65536);};
 auto deepId=Uuid().toString();folder.getChildFile("requests").getChildFile(deepId+".json").replaceWithText(String::repeatedString("[",200)+"0"+String::repeatedString("]",200));
 auto deep=waitResponse(deepId);check(str(deep,"error").contains("nesting"),"Deep JSON reached the recursive parser");
 auto junkId=Uuid().toString();folder.getChildFile("requests").getChildFile(junkId+".json").replaceWithText("{");
 check(str(waitResponse(junkId),"error").isNotEmpty(),"Malformed JSON produced no error");
 auto oldId=Uuid().toString();auto staged=folder.getChildFile(oldId+".staging");staged.replaceWithText(JSON::toString(object({{"id",oldId},{"method","read"},{"params",object({})}})));staged.setLastModificationTime(Time::getCurrentTime()-RelativeTime::seconds(90));check(staged.moveFileTo(folder.getChildFile("requests").getChildFile(oldId+".json")),"Cannot publish expired request");
 check(str(waitResponse(oldId),"error").contains("expired"),"Expired request was applied");
 auto again=Uuid().toString();folder.getChildFile("requests").getChildFile(again+".json").replaceWithText(JSON::toString(object({{"id",again},{"method","read"},{"params",object({})}})));
 check(str(get(waitResponse(again),"result"),"source").contains("# after apply"),"A bad request stopped the Live bridge");
 if(argc>1&&String(argv[1])=="--serve"){std::cout<<"IPC_ROOT="<<ipc.getFullPathName()<<std::endl;for(int i=0;i<1200;++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));}
 }
 check(ipc.findChildFiles(File::findDirectories,false).isEmpty(),"Destroyed instance left discovery state");temp.deleteRecursively();std::cout<<"Companion read, compile, render, guarded apply/restore and editor-independent IPC passed\n";return 0;}
 catch(const std::exception& e){std::cerr<<e.what()<<"\n";temp.deleteRecursively();return 1;}
}
