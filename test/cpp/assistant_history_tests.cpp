#include "app/AssistantHistory.hpp"
#include "app/Files.hpp"
#include "app/Text.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace flows;
using namespace flows::app;
using json::get;
using json::str;
namespace {
namespace fs = std::filesystem;
using String = std::string;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F&& action,const char* message){try{action();}catch(const std::exception&){return;}throw std::runtime_error(message);}
bool contains(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
std::string load(const fs::path& file){return files::tryRead(file,64*1024*1024).value_or("");}
struct Workspace {
    fs::path directory=fs::temp_directory_path()/("flows-history-tests-"+text::randomId());
    Workspace(){check(fs::create_directories(directory),"Could not create isolated workspace");}
    ~Workspace(){std::error_code ignored;fs::remove_all(directory,ignored);}
    fs::path history()const{return directory/".assistant-history.json";}
};
json::Value selected(AssistantHistory& history){return get(history.view(),"conversation");}
json::Value turn(AssistantHistory& history,int index){return (*get(selected(history),"messages").getArray())[static_cast<std::size_t>(index)];}
void persistence_and_lifecycle(){
    Workspace workspace;String id;
    {
        AssistantHistory history;history.setWorkspace(workspace.directory);check(history.error().empty(),"Empty history failed to load");
        id=history.create();check(history.activeId()==id,"Create did not select conversation");
        check(history.begin(id,"request-one","Explain this filter","bass.flows",7)==id,"Send targeted wrong conversation");
        rejects([&]{history.archive(id);},"Archived live request");rejects([&]{history.remove(id);},"Deleted live request");
        history.finish(id,"request-one","done","# Filter\n\nA **lowpass** keeps lower frequencies.","proposed source");
        check(str(selected(history),"title")=="Explain this filter","First prompt did not name conversation");
        check(history.proposalAvailable(id,"request-one","proposed source"),"Saved proposal lookup failed");
        history.rename(id,"Lowpass experiments");history.archive(id);
        check(!history.proposalAvailable(id,"request-one","proposed source"),"Archived proposal remained actionable");check(get(selected(history),"archived").asBool(),"Archive did not persist");
        rejects([&]{history.begin(id,"archived-send","hello","bass.flows",7);},"Sent to archive");
        history.restore(id);history.remove(id);check(get(selected(history),"deleted").asBool(),"Delete did not move to Trash");
        rejects([&]{history.begin(id,"trash-send","hello","bass.flows",7);},"Sent to Trash");history.restore(id);
    }
    {
        AssistantHistory history;history.setWorkspace(workspace.directory);check(history.activeId()==id,"Restart lost selection");
        auto conversation=selected(history);check(str(conversation,"title")=="Lowpass experiments","Restart lost renamed title");
        check(get(conversation,"messageCount").asInt()==1,"Restart lost message");
        auto message=turn(history,0);check(contains(str(message,"text"),"**lowpass**")&&str(message,"documentId")=="bass.flows"&&str(message,"proposedSource")=="proposed source","Restart lost full turn fields");
        check(get(message,"proposalAvailable").isNull(),"Persisted an actionable proposal token");
        rejects([&]{history.purge(id);},"Purged a conversation outside Trash");history.remove(id);history.purge(id);
        check(get(history.view(),"conversations").getArray()->empty(),"Permanent delete left conversation");
    }
}
void routed_completion_and_context(){
    Workspace workspace;AssistantHistory first,second;first.setWorkspace(workspace.directory);second.setWorkspace(workspace.directory);
    auto a=first.create("Instrument A"),b=second.create("Instrument B");
    first.begin(a,"first-running","Show A","a.flows",10);
    rejects([&]{first.begin(b,"first-running","Duplicate ID","b.flows",12);},"Accepted a reused request ID");
    first.select(a);check(str(turn(first,0),"status")=="working","Rejected duplicate ID interrupted the original request");
    first.select(b);
    second.begin(b,"second-running","Show B","b.flows",12);
    first.finish(a,"first-running","done","A response");second.finish(b,"second-running","done","B response");
    first.select(a);check(str(turn(first,0),"text")=="A response","Completion followed selected conversation instead of request");
    auto summaries=get(first.view(),"conversations");check(summaries.getArray()->size()==2,"Multiple instances overwrote each other's conversations");
    first.begin(a,"failed-turn","failed question","a.flows",13);first.finish(a,"failed-turn","failed","Provider failed");
    first.begin(a,"cancelled-turn","cancelled question","a.flows",14);first.finish(a,"cancelled-turn","cancelled","Cancelled");
    first.begin(a,"second-good","Continue A","c.flows",15);first.finish(a,"second-good","done","Second A response");
    auto context=first.context(a);check(context.size()==4,"Context included incomplete turns or missed completed turns");
    check(str(context[0],"role")=="user"&&str(context[1],"role")=="assistant"&&str(context[2],"role")=="user"&&str(context[3],"role")=="assistant","History roles are unpaired");
    check(contains(str(context[0],"content"),"a.flows")&&contains(str(context[2],"content"),"c.flows"),"Historical instrument context omitted");
    for(int i=0;i<18;++i){auto request="limit-"+std::to_string(i);first.begin(a,request,"question "+std::to_string(i),"a.flows",static_cast<std::uint64_t>(20+i));first.finish(a,request,"done","answer "+std::to_string(i));}
    context=first.context(a);check(context.size()==32&&str(context[0],"content").starts_with("question 2"),"Context did not keep most recent 16 successful turns");
}
void interrupted_recovery_and_corruption(){
    Workspace workspace;String id;
    {
        AssistantHistory live;live.setWorkspace(workspace.directory);id=live.create();live.begin(id,"unfinished","Continue later","a.flows",2);
        AssistantHistory another;another.setWorkspace(workspace.directory);another.select(id);check(str(turn(another,0),"status")=="working","A live owner's request was interrupted by another instance");
    }
    {
        AssistantHistory recovered;recovered.setWorkspace(workspace.directory);recovered.select(id);
        check(str(turn(recovered,0),"status")=="interrupted","Restart did not recover abandoned pending request");
        recovered.begin(id,"retry","Retry now","a.flows",2);recovered.finish(id,"retry","done","Retry completed");
    }
    const String corrupted="{ broken history";{std::ofstream out(workspace.history());out<<corrupted;check(bool(out),"Could not create corrupted fixture");}
    AssistantHistory blocked;blocked.setWorkspace(workspace.directory);check(!blocked.error().empty(),"Corrupted file was silently accepted");
    rejects([&]{blocked.create();},"Overwrote corrupted history");check(load(workspace.history())==corrupted,"Corruption handling destroyed original file");
    check(!str(blocked.view(),"historyError").empty(),"Corruption did not return actionable UI error");
}
void terminal_save_failure_recovery(){
    Workspace workspace;AssistantHistory history;history.setWorkspace(workspace.directory);const auto id=history.create();
    history.begin(id,"write-failed","Explain A","a.flows",1);
    const auto backup=workspace.directory/"preserved-history.json";
    check(files::move(workspace.history(),backup)&&fs::create_directory(workspace.history()),"Could not inject terminal history failure");
    rejects([&]{history.finish(id,"write-failed","done","Answer");},"Terminal save unexpectedly succeeded through an obstruction");
    rejects([&]{history.finish(id,"write-failed","failed","Save failed");},"Failure save unexpectedly succeeded through an obstruction");
    history.endRequest("write-failed");
    {std::error_code removed;fs::remove_all(workspace.history(),removed);check(!removed&&files::move(backup,workspace.history()),"Could not restore original history after failure");}
    check(str(turn(history,0),"status")=="interrupted","Terminal worker failure kept pending request alive indefinitely");
    history.begin(id,"retry-after-write","Retry","a.flows",1);history.finish(id,"retry-after-write","done","Recovered");
    history.archive(id);check(get(selected(history),"archived").asBool(),"Recovered conversation still cannot archive");
}
void capacity_limits(){
    Workspace workspace;AssistantHistory history;history.setWorkspace(workspace.directory);const auto id=history.create();
    for(int i=0;i<100;++i){const auto request="turn-"+std::to_string(i);history.begin(id,request,"Question","a.flows",1);history.finish(id,request,"done","Answer");}
    const auto before=load(workspace.history());rejects([&]{history.begin(id,"too-many","Question","a.flows",1);},"Exceeded per-conversation turn cap");
    check(load(workspace.history())==before,"Capacity failure erased saved conversation");
    const auto next=history.create();const String large(1024*1024,'x');
    history.begin(next,"large-1","question","a.flows",1);history.finish(next,"large-1","done",large,large);
    history.begin(next,"large-2","question","a.flows",1);history.finish(next,"large-2","done",large,large);
    history.begin(next,"large-3","question","a.flows",1);history.finish(next,"large-3","done",large,large);
    rejects([&]{history.begin(next,"over-bytes","question","a.flows",1);},"Ignored completion-space byte reservation");
    history.remove(next);history.purge(next);history.begin(history.create(),"after-purge","question","a.flows",1);
}
}
int main(){try{persistence_and_lifecycle();routed_completion_and_context();interrupted_recovery_and_corruption();terminal_save_failure_recovery();capacity_limits();std::cout<<"Assistant history passed: atomic persistence, rename/archive/Trash/restore/purge, multi-instance merge, correct completion routing, bounded paired context, interrupted recovery, corruption preservation and limits.\n";return 0;}catch(const std::exception& exception){std::cerr<<"Assistant history failure: "<<exception.what()<<'\n';return 1;}}
