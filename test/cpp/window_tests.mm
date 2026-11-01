#include "host/app/FlowsEditor.hpp"
#include "host/app/MacAppearance.hpp"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// This must be a GUI app with bundle ID audio.flows.standalone. A different test
// ID bypasses the production transparency guard and cannot cover this regression.
namespace {
using namespace juce;
using namespace flows::app;
using Clock = std::chrono::steady_clock;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

WKWebView* findWebView(NSView* view, unsigned depth = 0) {
    if (view == nil || depth > 64) return nil;
    if ([view isKindOfClass:[WKWebView class]]) return static_cast<WKWebView*>(view);
    for (NSView* child in view.subviews)
        if (auto* found = findWebView(child, depth + 1)) return found;
    return nil;
}

struct NativeIdentity {
    ComponentPeer* peer = nullptr;
    void* view = nullptr;
    NSInteger windowNumber = 0;
    bool operator==(const NativeIdentity&) const = default;
};

class TestWindow final : public DocumentWindow {
public:
    TestWindow(FlowsProcessor& processor, bool transparent)
        : DocumentWindow("Flows Window Tests", transparent ? Colours::transparentBlack
                                                          : Colour(0xff151719), allButtons) {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setContentOwned(processor.createEditor(), true);
        centreWithSize(1440, 920);
        setVisible(true);
    }
    void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    Component& editor() { return *getContentComponent(); }
    NSView* nativeView() const {
        auto* peer = getPeer();
        return peer == nullptr ? nil : (__bridge NSView*) peer->getNativeHandle();
    }
    NativeIdentity identity() const {
        auto* view = nativeView();
        return {getPeer(), (__bridge void*) view, view.window.windowNumber};
    }
    WKWebView* webView() const { return findWebView(nativeView()); }
};

class WindowTests final : public JUCEApplication, private Timer {
public:
    const String getApplicationName() override { return "Flows Window Tests"; }
    const String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const String&) override {}

    void initialise(const String& commandLine) override {
        try {
            const auto args = StringArray::fromTokens(commandLine, true);
            for (const auto& arg : args) {
                if (arg == "--keep-workspace") keepWorkspace_ = true;
                else if (arg == "--editor-layout-only") editorLayoutOnly_ = true;
                else if (arg.startsWith("--timeout-ms=")) {
                    timeoutMs_ = arg.fromFirstOccurrenceOf("=", false, false).getIntValue();
                    check(timeoutMs_ >= 1000 && timeoutMs_ <= 120000, "Timeout must be 1000..120000 ms.");
                } else if (arg.startsWith("--snapshot=")) {
                    const auto path = arg.fromFirstOccurrenceOf("=", false, false).unquoted();
                    check(File::isAbsolutePath(path) && path.endsWithIgnoreCase(".png"),
                          "Snapshot must be an absolute .png path.");
                    snapshotFile_ = File(path);
                } else throw std::runtime_error("Usage: Flows Window Tests [--timeout-ms=20000] [--editor-layout-only] [--keep-workspace] [--snapshot=/absolute/path.png]");
            }
            check([[NSBundle mainBundle].bundleIdentifier isEqualToString:@"audio.flows.standalone"],
                  "Test bundle ID must be audio.flows.standalone; otherwise the production path is untested.");
            workspace_ = File::getSpecialLocation(File::tempDirectory)
                .getNonexistentChildFile("flows-window-tests", {}, false);
            check(workspace_.createDirectory().wasOk(), "Could not create isolated test workspace.");
            setenv("FLOWS_AI_CONFIG_DIR", workspace_.getChildFile("test-global-ai").getFullPathName().toRawUTF8(), 1);
            check(setenv("FLOWS_WORKSPACE", workspace_.getFullPathName().toRawUTF8(), 1) == 0,
                  "Could not configure isolated test workspace.");
            // Keep the bridge running; this test is the real app. Point it at the
            // workspace so it does not publish into the user's LiveBridge folder.
            check(setenv("FLOWS_LIVE_SYNTH_DIR", workspace_.getChildFile(".live-bridge").getFullPathName().toRawUTF8(), 1) == 0,
                  "Could not isolate the Live bridge.");
            const auto historyMessage=object({{"id","markdown-window-turn"},{"prompt","Explain this signal flow"},
                {"text","## Signal flow\n\n**One oscillator** with `key.pitch`.\n\n- Play a note\n- Inspect the signal\n\n| Signal | Role |\n| --- | --- |\n| raw | Audio |\n\n```flows\nraw = osc(pitch: key.pitch, level: -6db)\n```\n\n<script>window.__unsafeMarkdown=true</script>"},
                {"status","done"},{"documentId","examples/trance-rolling-bass.flows"},{"baseRevision",1}});
            const auto historyConversation=object({{"id","markdown-window-conversation"},{"title","Markdown test"},
                {"archived",false},{"deleted",false},{"updatedAt",Time::currentTimeMillis()},
                {"messages",Array<var>{historyMessage}}});
            check(workspace_.getChildFile(".assistant-history.json").replaceWithText(JSON::toString(object({
                {"version",1},{"activeConversationId","markdown-window-conversation"},
                {"conversations",Array<var>{historyConversation}}}))),"Could not seed the assistant UI regression.");
            processor_ = std::make_unique<FlowsProcessor>(true);
            // Deliberately use an opaque root first. The adapter must reject it
            // without attempting the old setOpaque(false)/peer-recreation path.
            window_ = std::make_unique<TestWindow>(*processor_, false);
            baseline_ = window_->identity();
            started_ = Clock::now();
            startTimer(50);
        } catch (const std::exception& error) { finish(false, error.what()); }
    }

    void shutdown() override {
        stopTimer();
        alive_.reset();
        window_.reset();
        if (processor_ && pumpingAudio_) processor_->releaseResources();
        processor_.reset();
        if (workspace_ != File() && !keepWorkspace_ && passed_) workspace_.deleteRecursively();
    }

    void systemRequestedQuit() override { finish(false, "Test window closed before completion."); }

private:
    std::unique_ptr<FlowsProcessor> processor_;
    std::unique_ptr<TestWindow> window_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    var interfaceFonts_;
    void checkInterfaceFonts(const var& state, double scale) {
        const auto current = get(state, "interfaceFonts");
        check(interfaceFonts_.isArray() && current.isArray(), "Missing interface font measurements.");
        for (int i = 0; i < interfaceFonts_.size(); ++i) {
            const auto before = static_cast<double>(interfaceFonts_[i]);
            check(before > 0, "Missing interface text region.");
            check(std::abs(static_cast<double>(current[i]) - before * scale) < .02,
                  "Interface text did not scale with the editor.");
        }
    }
    File workspace_, snapshotFile_;
    NativeIdentity baseline_;
    WKWebView* webIdentity_ = nil; // Owned by this test's editor; never retained across teardown.
    Clock::time_point started_, documentReadyAt_{}, observationReadyAt_{};
    String lastPage_;
    String interactionAction_, originalSource_, originalPath_, graphSignature_, knobId_, switchedSource_;
    String resetId_, beforeReset_, defaultRegressionSource_, multiRightSource_, multiLeftDraft_, choiceSource_, choiceAnchorGeometry_;
    double resetValue_ = 0;
    double explorerWidth_ = 0, knobValue_ = 0;
    double layoutEditorWidth_ = 0, layoutEditorHeight_ = 0, layoutRightWidth_ = 0, layoutFlowHeight_ = 0;
    int knobFrom_ = 0, knobTo_ = 0;
    int timeoutMs_ = 20000;
    int stage_ = 0;
    int interactionStep_ = 0;
    bool evaluating_ = false, finished_ = false, passed_ = false, keepWorkspace_ = false;
    bool editorLayoutOnly_ = false, editorLayoutShort_ = false;
    bool pumpingAudio_ = false, noteSent_ = false, heldPixelsRecorded_ = false;
    std::uint64_t renderedSamples_ = 0, heldRenderedSamples_ = 0;
    juce::int64 observationEpoch_ = 0, captureResets_ = 0, heldCaptureEnd_ = 0, heldSequence_ = 0;
    String heldCapsuleHash_, heldInspectorHash_, testModelId_, pitchCapsuleHash_, pitchFlowHash_;
    juce::int64 keyCaptureEpoch_ = 0;
    String capturePath_ = "examples/sine-reference.flows";
    double stereoDifference_ = 0;

    void pumpCaptureAudio() {
        if (!pumpingAudio_) return;
        const auto snapshot = processor_->snapshot();
        const auto document = get(snapshot,"document"), compile = get(snapshot,"compile"), audio = get(snapshot,"audio");
        const bool installed = str(document,"path") == capturePath_ &&
            str(compile,"status") == "ready" && get(audio,"programRevision") == get(document,"revision");
        AudioBuffer<float> buffer(2,128);
        // No device is opened. Exercise the actual processor/capture path at a
        // bounded pace while WebKit and the Analyzer worker continue normally.
        for (int block = 0; block < 12; ++block) {
            MidiBuffer midi;
            if (installed && !noteSent_) { midi.addEvent(MidiMessage::noteOn(1,69,.8f),0); noteSent_ = true; }
            processor_->processBlock(buffer,midi);
            if (installed && capturePath_=="examples/stereo-motion.flows") for(int sample=0;sample<128;++sample) stereoDifference_=std::max(stereoDifference_,static_cast<double>(std::abs(buffer.getSample(0,sample)-buffer.getSample(1,sample))));
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < 128; ++sample)
                    check(std::isfinite(buffer.getSample(channel,sample)),"Capture regression rendered non-finite audio.");
            renderedSamples_ += 128;
        }
    }

    void timerCallback() override {
        if (finished_) return;
        try {
            pumpCaptureAudio();
            check(window_->identity() == baseline_, "Transparency changed the JUCE peer/native view/window identity.");
            if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count() > timeoutMs_)
                throw std::runtime_error("WebView did not become usable before timeout. Last page: " + lastPage_.toStdString());
            auto* web = window_->webView();
            if (web == nil) return;
            if (stage_ == 0) {
                check(!prepareTransparentWindow(window_->editor()), "Opaque window must be rejected without mutation.");
                check(window_->identity() == baseline_, "Opaque-window rejection recreated the peer.");
                check(window_->isOpaque(), "Opaque-window rejection changed JUCE alpha mode.");
                check(window_->webView() == web, "Opaque-window rejection replaced the WebView.");
                window_.reset();
                processor_->windowTransparency.store(false);
                window_ = std::make_unique<TestWindow>(*processor_, true);
                baseline_ = window_->identity();
                stage_ = 1;
                return;
            }
            check(!window_->isOpaque() && !window_->editor().isOpaque(), "Standalone alpha must be configured before WebView attachment.");
            check(prepareTransparentWindow(window_->editor()), "Production transparency adapter failed on an attached, preconfigured window.");
            check(window_->identity() == baseline_, "Adapter recreated a preconfigured transparent window.");
            auto* native = window_->nativeView();
            check(web.window == native.window && [web isDescendantOf:native], "WebView detached from its current native peer.");
            check(!web.hidden && web.bounds.size.width > 0 && web.bounds.size.height > 0, "Native WebView is hidden or has empty bounds.");
            check(!native.window.opaque && native.window.backgroundColor.alphaComponent == 0,
                  "Native window backing is not transparent.");
            check(native.window.alphaValue == 1, "Transparency must not fade text and controls through NSWindow alpha.");
            if (webIdentity_ == nil) webIdentity_ = web;
            check(web == webIdentity_, "Loaded WebView was replaced during transparency setup or resize.");
            if (evaluating_ || stage_ == 4) return;
            if (stage_ == 3) { pollInteractions(); return; }
            evaluating_ = true;
            const std::weak_ptr<int> token = alive_;
            auto* owner = this;
            [web evaluateJavaScript:@R"JS((() => {
                const bounds = selector => {
                    const element = document.querySelector(selector);
                    if (!element) return [0, 0];
                    const rect = element.getBoundingClientRect();
                    return [rect.width, rect.height];
                };
                return JSON.stringify({
                    url: location.href, ready: document.readyState,
                    app: bounds('.flows-app'), editor: bounds('.cm-editor'),
                    codeLength: document.querySelector('.cm-content')?.textContent?.length || 0,
                    bridge: !!window.__JUCE__?.backend &&
                        !!window.__JUCE__?.initialisationData?.__juce__functions?.includes('flowsRequest'),
                    connected: !document.querySelector('.disconnected-overlay')
                });
            })())JS" completionHandler:^(id result, NSError* error) {
                if (token.expired()) return;
                owner->pageResult(result, error);
            }];
        } catch (const std::exception& error) { finish(false, error.what()); }
    }

    void pageResult(id result, NSError* error) {
        evaluating_ = false;
        if (finished_) return;
        try {
            if (error != nil) { lastPage_ = String::fromUTF8([error.localizedDescription UTF8String]); return; }
            if (![result isKindOfClass:[NSString class]]) { lastPage_ = "JavaScript returned no page state"; return; }
            lastPage_ = String::fromUTF8([static_cast<NSString*>(result) UTF8String]);
            const auto state = JSON::parse(lastPage_);
            const auto app = get(state, "app"), editor = get(state, "editor");
            auto hasArea = [](const var& rect) {
                auto* values = rect.getArray();
                return values != nullptr && values->size() == 2 &&
                    static_cast<double>((*values)[0]) > 100 && static_cast<double>((*values)[1]) > 100;
            };
            if (str(state, "ready") != "complete" || !hasArea(app) || !hasArea(editor) ||
                !static_cast<bool>(get(state, "bridge")) || !static_cast<bool>(get(state, "connected")) ||
                static_cast<int>(get(state, "codeLength")) < 20) return;
            check(str(state, "url") != "about:blank", "Loaded instrument was replaced with about:blank.");
            check(window_->identity() == baseline_, "Peer changed while the page was loading.");
            if (stage_ == 1) {
                stage_ = 2;
                window_->setSize(1280, 800);
                return;
            }
            stage_ = 3;
            if (editorLayoutOnly_) { interactionAction_="editor-completion-audit"; interactionStep_=117; }
        } catch (const std::exception& exception) { finish(false, exception.what()); }
    }

    void takeSnapshot() {
        stage_ = 4;
        const std::weak_ptr<int> token = alive_;
        auto* owner = this;
        auto* configuration = [[WKSnapshotConfiguration alloc] init];
#if !__has_feature(objc_arc)
        [configuration autorelease];
#endif
        configuration.snapshotWidth = @640;
        configuration.afterScreenUpdates = YES;
        // WebKit snapshots only this test's own page, not the desktop or any
        // other application's window. This also checks actual raster output.
        [window_->webView() takeSnapshotWithConfiguration:configuration completionHandler:^(NSImage* image, NSError* snapshotError) {
            if (token.expired()) return;
            owner->snapshotResult(image, snapshotError);
        }];
    }

    // The literal has to end up holding the value the class declares, not merely
    // some value that happens to differ from what was there before.
    bool resetValueMatches(const var& compile) const {
        const auto parameters = get(compile, "parameters");
        if (auto* list = parameters.getArray()) for (const auto& parameter : *list)
            if (str(parameter, "id") == resetId_) return std::abs(static_cast<double>(get(parameter, "value")) - resetValue_) < 1e-6;
        return false;
    }

    void pollInteractions() {
        // Each action is dispatched once. Later timer ticks observe React/native
        // acknowledgements instead of depending on fixed sleeps or synchronous DOM updates.
        const auto action = std::exchange(interactionAction_, {});
        const auto script = String(R"JS(((action, selectedKnobId, phase) => {
            const visibleEditor = root => [...root.querySelectorAll('.cm-content')].find(element => element.getClientRects().length);
            const groups = [...document.querySelectorAll('.editor-group')].filter(element => element.getClientRects().length);
            const editor = visibleEditor(document.querySelector('.editor-group.active') || document);
            const graph = document.querySelector('.dataflow-viewport');
            const separator = document.querySelector('[role="separator"][aria-label="Resize explorer"]');
            const knobs = [...document.querySelectorAll('.flow-knob[role="slider"]:not([disabled])')];
            const knob = selectedKnobId ? knobs.find(element => element.dataset.parameterId === selectedKnobId)
                : knobs.find(element => +element.getAttribute('aria-valuenow') < +element.getAttribute('aria-valuemax'));
            const key = (target, key, code, metaKey = false) => {
                if (!target) throw new Error(`Missing target for ${action}`);
                target.focus();
                target.dispatchEvent(new KeyboardEvent('keydown', {key, code, metaKey, bubbles: true, cancelable: true}));
            };
            const choiceAnchorState = () => {
                const target = [...(editor?.querySelectorAll('.selectable-option') || [])].find(element => element.dataset.choiceId === selectedKnobId);
                const scroller = target?.closest('.cm-scroller');
                if (!target || !scroller) return {visible:false, geometry:[]};
                const box = target.getBoundingClientRect(), viewport = scroller.getBoundingClientRect();
                const x = box.x + box.width / 2, y = box.y + box.height / 2;
                return {
                    visible: box.width > 0 && box.height > 0 && x >= Math.max(0, viewport.left) && x < Math.min(innerWidth, viewport.right) &&
                        y >= Math.max(0, viewport.top) && y < Math.min(innerHeight, viewport.bottom) && target.contains(document.elementFromPoint(x, y)),
                    geometry: [box.x, box.y, box.width, box.height, scroller.scrollLeft, scroller.scrollTop]
                };
            };
            try {
                if (action === 'font-up') key(editor, '+', 'Equal', true);
                if (action === 'assistant-markdown') {
                    editor.focus();
                    if (document.querySelector('.ai-configuration')) document.querySelector('.agent-panel button[title="AI connection settings"]')?.click();
                }
                if (action === 'assistant-activity-audit') {
                    window.__assistantActivityAudit={done:false,error:''};
                    (async()=>{
                        const settle=()=>new Promise(resolve=>setTimeout(resolve,40));
                        const require=(ok,message)=>{if(!ok)throw new Error(message);};
                        const until=async(condition,message)=>{const end=performance.now()+3000;while(!condition()&&performance.now()<end)await settle();require(condition(),message);};
                        const backend=window.__JUCE__.backend;
                        for(const [id,kind,label,state] of [['thinking-0','thinking','Thinking','done'],['tool-0','tool','Reading source','done'],['tool-1','tool','Checking the code','failed']]) {
                            backend.emitByBackend('flowsAgent',JSON.stringify({requestId:'markdown-window-turn',conversationId:'markdown-window-conversation',status:'done',activity:{id,kind,label,state,detail:'Isolated UI regression',durationMs:320}}));
                        }
                        await until(()=>document.querySelector('.agent-activity-toggle')?.textContent.includes('3 steps'),'Activity events did not reach the rendered conversation');
                        const toggle=document.querySelector('.agent-activity-toggle');
                        require(toggle && toggle.getAttribute('aria-expanded')==='false','Completed activity did not start collapsed');
                        require(toggle.textContent.includes('3 steps'),'Activity did not retain all tool steps');
                        toggle.click();await settle();
                        require(document.querySelectorAll('.agent-activity-steps li').length===3,'Activity did not expand its steps');
                        require(document.querySelector('.activity-failed'),'Failed tool did not retain its result state');
                        require(!document.querySelector('.agent-activity-current'),'Completed answer retained a thinking indicator');
                        require(document.querySelector('.assistant-message h2')?.textContent==='Signal flow','Progress replaced the answer');
                        toggle.click();await settle();
                        require(!document.querySelector('.agent-activity-steps'),'Activity did not collapse');
                        window.__assistantActivityAudit.done=true;
                    })().catch(error=>{window.__assistantActivityAudit={done:true,error:String(error)};});
                }
                if (action === 'assistant-model-first') {
                    const select=document.querySelector('select[aria-label="Assistant model"]');
                    if(!select) throw new Error('Missing global model selector');
                    select.value=select.options[0].value;select.dispatchEvent(new Event('change',{bubbles:true}));
                }
                if (action === 'assistant-model-settings') document.querySelector('button[aria-label="Manage global models"]')?.click();
                if (action === 'assistant-model-close') document.querySelector('button[aria-label="AI connection settings"]')?.click();
                if (action === 'performance-observation-ready') {
                    const view=editor?.cmTile?.root?.view;
                    const anchor=view?.state.doc.toString().indexOf('raw =');
                    if(!view || anchor<0) throw new Error('Missing live performance editor');
                    editor.focus(); view.dispatch({selection:{anchor},scrollIntoView:true});
                    for(const [label,value] of [['Observation voice','-1'],['Observation channel','mid']]) {
                        const select=document.querySelector(`.dataflow-toolbar select[aria-label="${label}"]`);
                        if(!select) throw new Error(`Missing ${label} control`);
                        if(select.value!==value) {select.value=value;select.dispatchEvent(new Event('change',{bubbles:true}));}
                    }
                    const raw=document.querySelector('.flow-node[data-signal="raw"] .flow-node-title');
                    if(!raw) throw new Error('Missing live performance raw node');
                    raw.click();
                }
                if (action === 'assistant-history') document.querySelector('.agent-panel button[title="Conversation history"]')?.click();
                if (action.startsWith('assistant-folder:')) {
                    const name=action.split(':')[1];
                    const button=[...document.querySelectorAll('.agent-history-tabs button')].find(element=>element.textContent.toLowerCase()===name);
                    if (!button) throw new Error('Missing conversation folder');
                    button.click();
                }
                if (action.startsWith('assistant-row:')) {
                    const row=document.querySelector('[data-conversation-id="markdown-window-conversation"]');
                    const title={rename:'Rename conversation',archive:'Archive conversation',trash:'Move conversation to Trash',restore:'Restore conversation'}[action.split(':')[1]];
                    const button=row?.querySelector('button[title="'+title+'"]');
                    if (!button) throw new Error('Missing conversation action '+action);
                    button.click();
                }
                if (action === 'assistant-rename-save') {
                    const input=document.querySelector('input[aria-label="Conversation title"]');
                    if (!input) throw new Error('Missing inline conversation rename field');
                    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(input,'UI history renamed');
                    input.dispatchEvent(new Event('input',{bubbles:true}));
                }
                if (action === 'assistant-rename-submit') document.querySelector('.agent-history-rename')?.requestSubmit();
                if (action === 'assistant-open') document.querySelector('[data-conversation-id="markdown-window-conversation"] .agent-history-open')?.click();
                if (action === 'workspace-drop') {
                    const folder = document.querySelector('.folder-entry[title="AI-gen"]');
                    if (!folder) throw new Error('AI-gen folder is missing');
                    const transfer = new DataTransfer();
                    transfer.items.add(new File([editor.textContent], 'drop-ui.flows', {type:'text/plain'}));
                    folder.dispatchEvent(new DragEvent('drop', {bubbles:true,cancelable:true,dataTransfer:transfer}));
                }
                if (action === 'sequencer-open' || action === 'sequencer-close') document.querySelector('.sequencer-toggle')?.click();
                if (action.startsWith('sequencer-source:')) {
                    const select=document.querySelector('select[aria-label="Lane 1 sound source"]');
                    if (!select) throw new Error('Missing sequencer source selector');
                    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype,'value').set.call(select,action.substring('sequencer-source:'.length));
                    select.dispatchEvent(new Event('change',{bubbles:true}));
                }
                if (action === 'sequencer-gate') {
                    const select=document.querySelector('select[aria-label="Sample playback"]');
                    if (!select) throw new Error('Missing sample playback selector');
                    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype,'value').set.call(select,'gated');
                    select.dispatchEvent(new Event('change',{bubbles:true}));
                }
                if (action === 'sequencer-mix') {
                    const volume=document.querySelector('input[aria-label="Lane 1 volume"]');
                    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(volume,'-9');
                    volume.dispatchEvent(new Event('input',{bubbles:true}));
                    const division=document.querySelector('select[aria-label="Metronome interval"]');
                    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype,'value').set.call(division,'8');
                    division.dispatchEvent(new Event('change',{bubbles:true}));
                    document.querySelector('button[aria-label="Metronome enabled"]').click();
                }
                if (action === 'font-down') key(editor, '-', 'Minus', true);
                if (action === 'undo') key(editor, 'z', 'KeyZ', true);
                if (action === 'knob-up') key(knob, 'ArrowUp', 'ArrowUp');
                if (action === 'resize') key(separator, 'ArrowRight', 'ArrowRight');
                if (action === 'multi-split') key(editor, String.fromCharCode(92), 'Backslash', true);
                if (action === 'multi-focus-left' || action === 'multi-focus-right') {
                    const target = visibleEditor(groups[action === 'multi-focus-left' ? 0 : 1]);
                    if (!target) throw new Error('Missing split instrument editor');
                    target.focus();
                }
                if (action === 'multi-hold-and-focus-left') {
                    const toggle = document.querySelector('.typing-toggle');
                    if (!toggle?.classList.contains('selected')) toggle?.click();
                    // Wait for React's keyboard listener to observe the enabled
                    // setting before the following action starts the held key.
                }
                if (action === 'multi-key-down-and-focus-left') {
                    graph.focus();
                    const down = new KeyboardEvent('keydown', {key:'a',code:'KeyA',bubbles:true,cancelable:true});
                    graph.dispatchEvent(down); window.__flowsSwitchKey = {initial:down.defaultPrevented};
                    visibleEditor(groups[0]).focus();
                }
                if (action === 'multi-release-after-switch') {
                    const target = visibleEditor(groups[0]); target.focus();
                    const held = window.__flowsSwitchKey;
                    held.repeats = Array.from({length:5}, () => {
                        const event = new KeyboardEvent('keydown', {key:'a',code:'KeyA',repeat:true,bubbles:true,cancelable:true});
                        target.dispatchEvent(event); return event.defaultPrevented;
                    });
                    const up = new KeyboardEvent('keyup', {key:'a',code:'KeyA',bubbles:true,cancelable:true});
                    target.dispatchEvent(up); held.released = up.defaultPrevented;
                    document.querySelector('.typing-toggle.selected')?.click();
                }
                if (action === 'multi-edit-left-and-focus-right' || action === 'multi-edit-and-close') {
                    const left = visibleEditor(groups[0]), view = left?.cmTile?.root?.view;
                    if (!view) throw new Error('Missing left group CodeMirror view');
                    left.focus(); view.dispatch({changes:{from:view.state.doc.length,insert:'\n# Left group draft\n'}});
                    if (action === 'multi-edit-left-and-focus-right') visibleEditor(groups[1]).focus();
                    else key(left, 'w', 'KeyW', true);
                }
                if (action === 'multi-close-right-active') {
                    const tab = groups[1]?.querySelector('.editor-tab.active');
                    const close = tab?.querySelector('[data-tab-close],button[aria-label^="Close"]');
                    if (!close) throw new Error('Missing close button on right group active tab');
                    close.click();
                }
                if (action === 'multi-cancel-close' || action === 'multi-discard-close') {
                    const button = document.querySelector(`[data-close-action="${action === 'multi-cancel-close' ? 'cancel' : 'discard'}"]`);
                    if (!button) throw new Error('Missing unsaved-instrument close choice');
                    button.click();
                }
                if (action === 'multi-close-left') key(visibleEditor(groups[0]), 'w', 'KeyW', true);
                if (action === 'multi-reopen-empty') {
                    const empty = groups[1]?.querySelector('.empty-editor');
                    if (!empty) throw new Error('Closed editor group has no empty workspace');
                    empty.dispatchEvent(new PointerEvent('pointerdown', {bubbles:true,cancelable:true}));
                    const file = [...document.querySelectorAll('.file-entry')].find(element => element.title === 'examples/probe-lab.flows');
                    if (!file) throw new Error('Missing instrument to reopen in the empty group');
                    file.click();
                    if (document.querySelector('.ai-configuration')) document.querySelector('.agent-panel button[title="AI connection settings"]')?.click();
                }
                if (action === 'panels-hide-all') {
                    const toggles = [...document.querySelectorAll('.app-header [data-panel-toggle]')];
                    if (toggles.length !== 4 || toggles.some(button => !button.title || !button.getAttribute('aria-controls') || !button.getAttribute('aria-label')))
                        throw new Error('All four panel toggles need a header button, tooltip and accessible label');
                    window.__flowsLayoutNodes = ['explorer-panel','assistant-panel','dataflow-panel'].map(id => document.getElementById(id));
                    const configuration = document.querySelector('.agent-panel button[title="AI connection settings"]');
                    if (!document.querySelector('.ai-configuration')) configuration?.click();
                    for (const button of toggles) if (button.getAttribute('aria-expanded') === 'true') button.click();
                }
                if (action === 'panels-shortcuts') {
                    const consumed = [];
                    const dispatch = (key, code, extra = {}) => {
                        const event = new KeyboardEvent('keydown', {key, code, metaKey: true, bubbles: true, cancelable: true, ...extra});
                        editor.focus(); editor.dispatchEvent(event); return event.defaultPrevented;
                    };
                    for (const [key, code, extra] of [['b','KeyB',{}],['∫','KeyB',{altKey:true}],['j','KeyJ',{}],['J','KeyJ',{shiftKey:true}]]) {
                        consumed.push(dispatch(key,code,extra));
                        consumed.push(dispatch(key,code,{...extra,repeat:true}));
                    }
                    window.__flowsLayoutShortcuts = consumed;
                }
                if (action === 'computer-hide' || action === 'computer-show') {
                    const collapse = document.querySelector('.keyboard-collapse');
                    if (!collapse) throw new Error('Piano visibility toggle is missing');
                    if ((collapse.getAttribute('aria-expanded') === 'true') !== (action === 'computer-show')) collapse.click();
                }
                if (action === 'computer-enable') {
                    const toggle = document.querySelector('.typing-toggle');
                    if (!toggle) throw new Error('Computer keyboard toggle is missing');
                    const backend = window.__JUCE__.backend, emit = backend.emitEvent;
                    const record = window.__flowsKeyRegression = {on: 0, off: 0, pitches: [], repeats: [], editing: []};
                    // Observe this test page's real outbound requests. Forward
                    // each one unchanged so note handling still reaches native.
                    backend.emitEvent = function(name, payload) {
                        const method = name === '__juce__invoke' && payload?.params?.[0]?.method;
                        if (method === 'keyboard.noteOn') { ++record.on; record.pitches.push(payload.params[0].params.pitch); }
                        if (method === 'keyboard.noteOff') ++record.off;
                        return emit.call(this, name, payload);
                    };
                    record.restore = () => { backend.emitEvent = emit; };
                    if (!toggle.classList.contains('selected')) document.dispatchEvent(new KeyboardEvent('keydown', { key: 'k', code: 'KeyK', metaKey: true, bubbles: true, cancelable: true }));
                }
                if (action === 'computer-hold' || action === 'computer-release') {
                    const record = window.__flowsKeyRegression;
                    const dispatch = (target, type, key, code, repeat = false, metaKey = false) => {
                        if (!target) throw new Error('Missing keyboard test focus target');
                        target.focus();
                        const event = new KeyboardEvent(type, {key, code, repeat, metaKey, bubbles: true, cancelable: true});
                        target.dispatchEvent(event);
                        return event.defaultPrevented;
                    };
                    if (action === 'computer-hold') {
                        record.readonly = editor?.getAttribute('contenteditable') === 'false';
                        const view = editor?.cmTile?.root?.view;
                        if (view) { const before = view.state.doc.toString(); view.dispatch({changes:{from:0,insert:'# forbidden edit\n'}}); record.editBlocked = view.state.doc.toString() === before; }
                        record.initial = dispatch(editor, 'keydown', 'a', 'KeyA');
                        for (let repeat = 0; repeat < 30; ++repeat) record.repeats.push(dispatch(graph, 'keydown', 'a', 'KeyA', true));
                        record.editing = [dispatch(document.querySelector('.file-search input'), 'keydown', 's', 'KeyS'), dispatch(document.querySelector('.file-search input'), 'keydown', 's', 'KeyS', true)];
                        record.shortcut = dispatch(graph, 'keydown', 'd', 'KeyD', false, true);
                    } else record.released = dispatch(graph, 'keyup', 'a', 'KeyA');
                }
                if (action === 'octave-audit') {
                    const record = window.__flowsKeyRegression;
                    const frame = () => new Promise(resolve => setTimeout(resolve, 40));
                    const dispatch = (key, type = 'keydown', extra = {}, target = graph) => {
                        const event = new KeyboardEvent(type, {key, code:`Key${key.toUpperCase()}`, bubbles:true, cancelable:true, ...extra});
                        target.dispatchEvent(event); return event.defaultPrevented;
                    };
                    const check = (ok, message) => { if (!ok) throw new Error(message); };
                    void (async () => {
                        check(dispatch('x'), 'Hidden piano did not accept octave up');
                        check(dispatch('x','keyup'), 'Octave keyup was not consumed');
                        await frame();
                        dispatch('a'); check(record.pitches.at(-1) === 48, 'X did not raise the next note by 12 semitones');
                        const off = record.off;
                        check(dispatch('z'), 'Octave down was not consumed');
                        check(record.off === off + 1, 'Changing octave did not release the held note');
                        dispatch('z','keyup'); dispatch('a','keyup'); await frame();
                        dispatch('a'); check(record.pitches.at(-1) === 36, 'Z did not lower the next note by 12 semitones'); dispatch('a','keyup');
                        for (const [key, pitch] of [['x',84],['z',12]]) {
                            dispatch(key);
                            for (let n=0; n<40; ++n) check(dispatch(key,'keydown',{repeat:true}), 'Held octave repeat was not consumed');
                            check(dispatch(key,'keyup',{},document.querySelector('.file-search input')), 'Octave keyup lost ownership after focus changed');
                            await frame(); dispatch('a'); check(record.pitches.at(-1) === pitch, 'Long-press octave limit or batched next-note pitch is wrong'); dispatch('a','keyup');
                        }
                        const input = document.querySelector('.file-search input');
                        check(!dispatch('x','keydown',{},input), 'Octave shortcut intercepted text entry');
                        check(!dispatch('z','keydown',{metaKey:true}), 'Octave shortcut intercepted Command+Z');
                        check(!dispatch('x','keydown',{ctrlKey:true}), 'Octave shortcut intercepted Control+X');
                        check(!dispatch('x','keydown',{isComposing:true}), 'Octave shortcut intercepted IME input');
                        document.querySelector('.typing-toggle').click(); await frame();
                        check(!dispatch('x'), 'Octave shortcut was active in Edit mode');
                        document.querySelector('.typing-toggle').click(); await frame();
                        dispatch('a'); check(record.pitches.at(-1) === 12, 'Excluded input changed the octave'); dispatch('a','keyup');
                        dispatch('x'); dispatch('x','keyup'); dispatch('x'); dispatch('x','keyup'); await frame();
                        document.querySelector('.keyboard-collapse').click(); await frame();
                        check(document.querySelector('.octave span')?.textContent === 'C2\u2014B3', 'Piano octave display did not follow keyboard shortcuts: ' + document.querySelector('.octave span')?.textContent);
                        record.octaveDone = true;
                    })().catch(error => { record.octaveError = String(error); record.octaveDone = true; });
                }
                if (action === 'computer-cleanup') {
                    window.__flowsKeyRegression?.restore();
                    const toggle = document.querySelector('.typing-toggle.selected');
                    if (toggle) toggle.click();
                    const collapse = document.querySelector('.keyboard-collapse[aria-expanded="false"]');
                    if (collapse) collapse.click();
                }
                if (action.startsWith('probe-scroll:')) {
                    document.querySelector('.signal-modal .modal-title button')?.click();
                    const view = editor?.cmTile?.root?.view, name = action.slice(13);
                    const anchor = view?.state.doc.toString().indexOf(`${name} =`);
                    if (!view || anchor < 0) throw new Error(`Missing probe source ${name}`);
                    view.dispatch({selection:{anchor},scrollIntoView:true});
                }
                if (action.startsWith('probe-inspect:')) {
                    const name = action.slice(14);
                    const capsule = [...editor.querySelectorAll('.signal-capsule')].find(element => element.querySelector('span')?.textContent === name);
                    if (!capsule) throw new Error(`Missing ${name} capsule`);
                    capsule.click();
                }
                if (action === 'probe-spectrum') {
                    const plots=[...document.querySelectorAll('.signal-modal .signal-plot')];
                    if (plots.map(p=>p.dataset.viewMode).join(',')!=='wave,spectrum') throw new Error('Inspector must show waveform above spectrum');
                }
                if (action === 'probe-listen') document.querySelector('.signal-modal [aria-label="Listen to p_after"]')?.click();
                if (action.startsWith('probe-channel:')) {
                    const select = document.querySelector('.signal-modal select[aria-label="Observation channel"]');
                    if (!select) throw new Error('Missing stereo observation channel');
                    select.value = action.slice(14); select.dispatchEvent(new Event('change', {bubbles:true}));
                }
                if (action === 'tempo-edit') {
                    const input = document.querySelector('input[aria-label="Tempo BPM"]');
                    if (!input || input.disabled) throw new Error('Standalone tempo input is unavailable');
                    input.focus(); Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, '120');
                    input.dispatchEvent(new Event('input', {bubbles:true}));
                }
                if (action === 'tempo-commit') document.querySelector('input[aria-label="Tempo BPM"]')?.blur();
                if (action === 'probe-voice-one') {
                    const select = document.querySelector('.signal-modal select[aria-label="Observation voice"]');
                    if (!select) throw new Error('Missing observation voice selection');
                    select.value = '0'; select.dispatchEvent(new Event('change', {bubbles:true}));
                }
                if (action === 'inspect-raw') {
                    const capsule = [...editor.querySelectorAll('.signal-capsule')].find(element => element.querySelector('span')?.textContent === 'raw');
                    if (!capsule) throw new Error('Missing raw capsule');
                    capsule.click();
                }
                if (action === 'close-inspector') {
                    const close = document.querySelector('.signal-modal .modal-title button');
                    if (!close) throw new Error('Missing Inspector close button');
                    close.click();
                }
                if (action === 'hold-observation' || action === 'resume-observation') {
                    const label = action === 'hold-observation' ? 'Hold observation' : 'Resume observation';
                    const button = document.querySelector(`.signal-modal button[aria-label="${label}"]`) || document.querySelector(`button[aria-label="${label}"]`);
                    if (!button) throw new Error(`Missing ${label} button`);
                    button.click();
                }
                if (action.startsWith('open:')) {
                    const file = [...document.querySelectorAll('.file-entry')].find(element => element.title === action.slice(5));
                    if (!file) throw new Error(`Missing explorer file ${action.slice(5)}`);
                    file.click();
                }
                if (action === 'choice-scroll') {
                    const view = editor?.cmTile?.root?.view, match = view?.state.doc.toString().match(/shape:\s*\w+/);
                    if (!view || !match) throw new Error('Missing authored LFO shape');
                    const anchor = match.index + match[0].length;
                    editor.focus({preventScroll:true});
                    view.dispatch({selection:{anchor}});
                    const target=[...editor.querySelectorAll('.selectable-option')].find(element=>element.dataset.choiceId===selectedKnobId);
                    if (!target) throw new Error('Missing option to scroll into view');
                    target.scrollIntoView({block:'center',inline:'nearest'});
                }
                if (action === 'choice-editor') {
                    const target = [...editor.querySelectorAll('.selectable-option')].find(element => element.dataset.choiceId === selectedKnobId);
                    if (!target) throw new Error('Missing compiler-bound choice in code');
                    if (!choiceAnchorState().visible) throw new Error('Code choice anchor moved outside the clickable viewport');
                    const box = target.getBoundingClientRect();
                    target.dispatchEvent(new PointerEvent('pointerdown', {pointerId:2091,pointerType:'mouse',button:0,buttons:1,clientX:box.x+box.width/2,clientY:box.y+box.height/2,bubbles:true,cancelable:true}));
                    window.dispatchEvent(new PointerEvent('pointerup', {pointerId:2091,button:0,buttons:0,bubbles:true}));
                }
                if (action === 'choice-flow') {
                    document.querySelector('.flow-fit')?.click();
                    const target = [...document.querySelectorAll('.flow-node .choice-select-button')].find(element => element.dataset.choiceId === selectedKnobId);
                    if (!target || target.disabled) throw new Error('Missing active Signal Flow choice: '+selectedKnobId+'; '+(document.querySelector('.flow-pending')?.textContent || 'ready'));
                    target.click();
                }
                if (action.startsWith('choice-pick:')) {
                    const value = action.slice(12), option = [...document.querySelectorAll('.choice-menu [role="option"]')].find(element=>element.dataset.value===value);
                    if (!option || option.getAttribute('aria-disabled') === 'true') throw new Error('Choice unavailable: '+value);
                    option.click();
                }
                if (action === 'choice-escape') key(document.querySelector('.choice-menu'),'Escape','Escape');
                if (action === 'choice-hide-flow') key(document.querySelector('.choice-menu'),'j','KeyJ',true);
                if (action === 'choice-show-flow') key(editor,'j','KeyJ',true);
                if (action === 'choice-focus-code') editor.focus();
                if (action === 'choice-keyboard') {
                    const menu = document.querySelector('.choice-menu');
                    for (const letter of 'square') key(menu,letter,'Key'+letter.toUpperCase());
                    key(menu,'Enter','Enter');
                }
                if (action === 'editor-completion-audit') {
                    window.__editorAudit={done:false,error:''};
                    (async()=>{
                        const settle=()=>new Promise(resolve=>setTimeout(resolve,160));
                        const require=(ok,message)=>{if(!ok)throw new Error(message);};
                        if(document.querySelector('.typing-toggle')?.getAttribute('aria-pressed')==='true')document.querySelector('.typing-toggle').click();
                        await settle();
                        const view=editor.cmTile?.root?.view;
                        require(view&&!view.state.readOnly,'Edit mode did not enable the source');
                        const original=view.state.doc.toString();
                        const at=original.lastIndexOf('    return');
                        require(at>=0,'Missing return for completion fixture');
                        const insert='    inspected = key.pi';
                        view.focus();view.dispatch({changes:{from:at,insert:insert+'\n'},selection:{anchor:at+insert.length},userEvent:'input.type'});
                        await settle();await settle();
                        const options=[...document.querySelectorAll('.cm-tooltip-autocomplete [role=option]')];
                        require(options.length===1&&options[0].textContent.includes('pitch'),'key.pi did not offer only the pitch field on unfinished code');
                        key(editor,'Tab','Tab');await settle();
                        require(view.state.doc.toString()===original.slice(0,at)+'    inspected = key.pitch\n'+original.slice(at),'Completion changed the wrong source range');
                        require(!document.querySelector('.cm-tooltip-autocomplete'),'Tab did not accept and close completion');
                        const savedSize=document.documentElement.style.getPropertyValue('--editor-size');
                        const savedFont=document.documentElement.style.getPropertyValue('--editor-font');
                        let checked=0;
                        for(const [font,size] of [['Menlo',16],['Monaco',23],['SFMono-Regular, monospace',32],['Menlo',10],['Menlo',16]]) {
                            document.documentElement.style.setProperty('--editor-font',font);
                            document.documentElement.style.setProperty('--editor-size',size+'px');
                            await settle();
                            // Compare rendered glyphs, not just row boxes: an inline
                            // preview can shift the source baseline within a row.
                            const checkGutters=()=>{ for(const row of editor.querySelectorAll('.cm-line')) {
                                const rowBox=row.getBoundingClientRect(),viewport=view.scrollDOM.getBoundingClientRect();
                                if(rowBox.top<viewport.top||rowBox.bottom>viewport.bottom)continue;
                                const walker=document.createTreeWalker(row,NodeFilter.SHOW_TEXT);
                                let node;while((node=walker.nextNode())&&!node.textContent.trim()){}
                                if(!node)continue;
                                const pos=view.posAtDOM(node,0),number=view.state.doc.lineAt(pos).number;
                                const gutter=[...view.dom.querySelectorAll('.cm-lineNumbers .cm-gutterElement')].find(e=>e.textContent===String(number)&&e.style.visibility!=='hidden');
                                if(!gutter)continue;
                                const sourceRange=document.createRange(),gutterRange=document.createRange();
                                sourceRange.setStart(node,0);sourceRange.setEnd(node,1);
                                gutterRange.selectNodeContents(gutter);
                                const sourceBox=sourceRange.getBoundingClientRect(),gutterBox=gutterRange.getBoundingClientRect();
                                require(Math.abs(sourceBox.top-gutterBox.top)<1.5,'Line '+number+' baseline differs by '+(sourceBox.top-gutterBox.top)+'px in '+font+' '+size+'; row='+row.getBoundingClientRect().height+' gutter='+gutter.getBoundingClientRect().height+' source='+sourceBox.top+' number='+gutterBox.top);
                            }};
                            checkGutters();
                            for(const scrollTop of [0,view.scrollDOM.scrollHeight]) {
                                view.scrollDOM.scrollTop=scrollTop;await settle();
                                view.coordsAtPos(view.state.selection.main.head);checkGutters();
                            }
                            for(let line=1;line<=view.state.doc.lines;++line) {
                                const text=view.state.doc.line(line);
                                for(let pos=text.from;pos<text.to;pos+=Math.max(1,Math.floor(text.length/6))) {
                                    const box=view.coordsAtPos(pos);
                                    if(!box||box.top<view.scrollDOM.getBoundingClientRect().top||box.bottom>view.scrollDOM.getBoundingClientRect().bottom)continue;
                                    const dom=view.domAtPos(pos),range=document.createRange();
                                    range.setStart(dom.node,dom.offset);range.setEnd(dom.node,dom.offset);
                                    const actual=range.getBoundingClientRect();
                                    if(actual.height===0)continue;
                                    require(Math.abs(actual.left-box.left)<1.5&&Math.abs(actual.top-box.top)<2,'Cursor coordinate diverged from the DOM text at '+pos+' in '+font+' '+size);
                                    const back=view.posAtCoords({x:box.left,y:(box.top+box.bottom)/2});
                                    require(back===pos,'Click position did not round-trip to the same source character');
                                    ++checked;
                                }
                            }
                        }
                        document.documentElement.style.setProperty('--editor-font',savedFont);
                        document.documentElement.style.setProperty('--editor-size',savedSize);
                        require(checked>60,'Geometry regression did not inspect enough source positions');
                        // A normal typed edit at a measured cursor must insert
                        // precisely there, even after inline widgets rebuild.
                        const position=view.state.doc.toString().indexOf('key.pitch',at)+4;
                        view.dispatch({selection:{anchor:position}});await settle();
                        const expected=view.state.doc.toString();
                        view.dispatch({changes:{from:view.state.selection.main.head,insert:'x'},selection:{anchor:position+1},userEvent:'input.type'});
                        await settle();require(view.state.doc.toString()===expected.slice(0,position)+'x'+expected.slice(position),'Typed text and the selected cursor diverged');
                        view.dispatch({changes:{from:0,to:view.state.doc.length,insert:original},selection:{anchor:0},userEvent:'input.type'});
                        await settle();
                        window.__editorAudit={done:true,error:'',checked};
                    })().catch(error=>{window.__editorAudit={done:true,error:String(error)};});
                }
                if (action === 'display-audit') {
                    window.__displayAudit={done:false,error:''};
                    (async()=>{
                        const settle=()=>new Promise(resolve=>setTimeout(resolve,40));
                        const require=(ok,message)=>{if(!ok)throw new Error(message);};
                        const capsule=[...editor.querySelectorAll('.signal-capsule')].find(e=>e.querySelector('span')?.textContent==='raw');
                        const scope=document.querySelector('.flow-node[data-signal="raw"] .flow-scope');
                        require(capsule?.dataset.viewMode==='spectrum' && scope?.dataset.viewMode==='spectrum','Audio previews must default to spectrum');
                        capsule.querySelector('canvas').click(); await settle();
                        const menu=document.querySelector('.signal-display-menu');
                        require(menu && menu.querySelector('[data-mode=spectrum][aria-checked=true]'),'Clicking the capsule spectrum must open display choices');
                        menu.querySelector('[data-mode=wave]').click(); await settle();
                        require(capsule.dataset.viewMode==='wave' && scope.dataset.viewMode==='wave','Waveform selection did not synchronize previews');
                        scope.click(); await settle();
                        document.querySelector('.signal-display-menu [data-mode=spectrum]').click(); await settle();
                        require(capsule.dataset.viewMode==='spectrum' && scope.dataset.viewMode==='spectrum','Graph spectrum selection did not synchronize previews');
                        capsule.click(); await settle();
                        const plots=[...document.querySelectorAll('.signal-modal .signal-plot')];
                        require(plots.map(p=>p.dataset.viewMode).join(',')==='wave,spectrum','Expanded signal must show waveform above spectrum');
                        require(plots.every(plot=>plot.querySelector('canvas')),'Expanded signal lost a canvas');
                        const scopeButtons=[...plots[0].querySelectorAll('.scope-modes button')];
                        require(scopeButtons.length===2 && !scopeButtons[0].disabled && plots[0].dataset.scopeMode==='beat','Raw audio must expose transport-aligned beat scope');
                        scopeButtons[1].click();await settle();
                        require(plots[0].dataset.scopeMode==='detail','Detail scope did not switch');
                        scopeButtons[0].click();await settle();
                        require(plots[0].dataset.scopeMode==='beat','Beat scope did not restore');
                        document.querySelector('.signal-modal .modal-title button').click(); await settle();
                        window.__displayAudit.done=true;
                    })().catch(error=>{window.__displayAudit={done:true,error:String(error)};});
                }
                if (action === 'gesture-audit') {
                    window.__gestureAudit = { done: false, error: '' };
                    (async () => {
                        const frame = () => new Promise(resolve => setTimeout(resolve, 16));
                        const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
                        const require = (condition, message) => { if (!condition) throw new Error(message); };
                        const until = async (condition,message) => { const end=performance.now()+2000; while(!condition() && performance.now()<end) await wait(20); require(condition(),message); };
                        const source = () => editor.cmTile.root.view.state.doc.toString();
                        const byId = id => [...document.querySelectorAll('.flow-node .flow-knob')].find(k=>k.dataset.parameterId===id);
                        const target = knobs.find(k=>+k.getAttribute('aria-valuemax')>+k.getAttribute('aria-valuemin') && +k.getAttribute('aria-valuenow')<+k.getAttribute('aria-valuemax'));
                        require(target, 'Missing knob for gesture audit');
                        const id=target.dataset.parameterId, original=source(), initial=+target.getAttribute('aria-valuenow');
                        const rect=target.getBoundingClientRect(), x=rect.x+rect.width/2, y=rect.y+rect.height/2;
                        const pointer=(element,type,offset,shift=false) => element.dispatchEvent(new PointerEvent(type,{pointerId:7001,pointerType:'mouse',isPrimary:true,button:0,buttons:type==='pointerup'?0:1,clientX:x,clientY:y-offset,shiftKey:shift,bubbles:true,cancelable:true}));
                        pointer(target,'pointerdown',0);
                        for(let step=1;step<=12;step++) { pointer(window,'pointermove',step*2); await frame(); }
                        require(document.body.classList.contains('flow-knob-dragging'),'Intermediate own source update ended drag prematurely');
                        const finalValue=+byId(id).getAttribute('aria-valuenow');
                        require(finalValue!==initial && source()!==original,'Continuous drag did not change value/source');
                        const finalSource=source();
                        pointer(window,'pointerup',24); await frame();
                        require(+byId(id).getAttribute('aria-valuenow')===finalValue,'Knob jumped back after pointer-up');
                        require(source()===finalSource,'Source jumped back after pointer-up');
                        await wait(180);
                        require(+byId(id).getAttribute('aria-valuenow')===finalValue,'Compilation replaced the committed value');
                        key(editor,'z','KeyZ',true); await wait(180); await frame();
                        require(source()===original,'A continuous drag did not undo as one edit');
                        require(+byId(id).getAttribute('aria-valuenow')===initial,'Undo did not restore graph value');
                        pointer(byId(id),'pointerdown',0);
                        pointer(window,'pointermove',40,true); await frame();
                        key(window,'Escape','Escape'); await wait(180); await frame();
                        require(source()===original && +byId(id).getAttribute('aria-valuenow')===initial,'Escape did not restore source and knob');
                        require(!document.body.classList.contains('flow-knob-dragging'),'Escape left an active drag');
                        // The same travel in source must produce the same value as
                        // the graph, and update every copy of its knob immediately.
                        const findNumber=()=>[...editor.querySelectorAll('.draggable-number')].find(n=>n.dataset.parameterId===id);
                        // CodeMirror virtualizes lines. Locate the source control
                        // at any text size instead of assuming it is already in DOM.
                        const scroller=editor.cmTile.root.view.scrollDOM;
                        for(let top=0;!findNumber() && top<scroller.scrollHeight;top+=Math.max(1,scroller.clientHeight/2)) {
                            scroller.scrollTop=top; await wait(40);
                        }
                        const number=findNumber();
                        require(number, 'Missing source number for cross-surface drag');
                        number.scrollIntoView({block:'center',inline:'center'}); await frame();
                        const nr=number.getBoundingClientRect(), nx=nr.x+nr.width/2, ny=nr.y+nr.height/2;
                        const numberPointer=(element,type,offset) => element.dispatchEvent(new PointerEvent(type,{pointerId:7002,pointerType:'mouse',isPrimary:true,button:0,buttons:type==='pointerup'?0:1,clientX:nx+offset,clientY:ny,bubbles:true,cancelable:true}));
                        numberPointer(number,'pointerdown',0);
                        numberPointer(window,'pointermove',24); await frame();
                        await until(()=>+byId(id).getAttribute('aria-valuenow')===finalValue,'Code and graph use different drag responses');
                        numberPointer(window,'pointerup',24); await wait(180); await frame();
                        require(source()===finalSource && +byId(id).getAttribute('aria-valuenow')===finalValue,'Code drag did not synchronize source/graph');
                        key(editor,'z','KeyZ',true);
                        await until(()=>source()===original && +byId(id).getAttribute('aria-valuenow')===initial,'Code drag did not undo as one edit');
                        // Exact input shares source ownership, validation and undo.
                        const valueButton=()=>byId(id).closest('.flow-parameter').querySelector('.flow-value-input');
                        valueButton().click(); await frame();
                        let entry=document.querySelector('.parameter-input-popup');
                        require(entry, 'Clicking a knob value did not open precise input');
                        let input=entry.querySelector('input'); input.value='1e999';
                        entry.dispatchEvent(new Event('submit',{bubbles:true,cancelable:true})); await frame();
                        require(entry.querySelector('[role="alert"]').textContent && source()===original, 'Invalid precise input edited source');
                        const suffix=valueButton().querySelector('small').textContent;
                        input.value=String(finalValue)+suffix;
                        entry.dispatchEvent(new Event('submit',{bubbles:true,cancelable:true})); await wait(180); await frame();
                        require(!document.querySelector('.parameter-input-popup') && source()===finalSource, 'Precise input did not commit the expected literal');
                        require(+byId(id).getAttribute('aria-valuenow')===finalValue,'Precise input did not synchronize the knob');
                        key(editor,'z','KeyZ',true);
                        await until(()=>source()===original && +byId(id).getAttribute('aria-valuenow')===initial,'Precise input was not a single undoable edit');
                        byId(id).closest('.flow-node').querySelector('.flow-node-title').click();
                        await until(()=>[...editor.querySelectorAll('.draggable-number')].some(n=>n.dataset.parameterId===id),'Source parameter did not become visible after reveal');
                        let inputNumber=[...editor.querySelectorAll('.draggable-number')].find(n=>n.dataset.parameterId===id);
                        inputNumber.scrollIntoView({block:'center',inline:'center'});await frame();
                        inputNumber=[...editor.querySelectorAll('.draggable-number')].find(n=>n.dataset.parameterId===id);
                        const ir=inputNumber.getBoundingClientRect();
                        inputNumber.dispatchEvent(new PointerEvent('pointerdown',{pointerId:7003,pointerType:'mouse',isPrimary:true,button:0,clientX:ir.x+ir.width/2,clientY:ir.y+ir.height/2,altKey:true,bubbles:true,cancelable:true}));
                        await frame();entry=document.querySelector('.parameter-input-popup');
                        require(entry,'Alt-click in code did not open precise input');
                        input=entry.querySelector('input');input.value=String(finalValue);
                        key(input,'Escape','Escape');await frame();
                        require(!document.querySelector('.parameter-input-popup') && source()===original,'Cancel precise input changed source');
                        require(document.activeElement===editor,'Closing source precise input did not restore editor focus');
                        inputNumber.dispatchEvent(new PointerEvent('pointerdown',{pointerId:7005,pointerType:'mouse',isPrimary:true,button:0,clientX:ir.x+ir.width/2,clientY:ir.y+ir.height/2,altKey:true,bubbles:true,cancelable:true}));await frame();
                        entry=document.querySelector('.parameter-input-popup');require(entry,'Could not reopen source precise input');
                        entry.querySelector('input').value=String(finalValue)+suffix;
                        entry.dispatchEvent(new Event('submit',{bubbles:true,cancelable:true}));
                        await until(()=>source()===finalSource && +byId(id).getAttribute('aria-valuenow')===finalValue,'Source precise input did not commit to code and graph');
                        await wait(180);key(editor,'z','KeyZ',true);
                        await until(()=>source()===original && +byId(id).getAttribute('aria-valuenow')===initial,'Source precise input did not undo as one edit');

                        // Horizontal and vertical knob motion use the same response.
                        const horizontal=byId(id);
                        horizontal.dispatchEvent(new PointerEvent('pointerdown',{pointerId:7004,pointerType:'mouse',isPrimary:true,button:0,clientX:x,clientY:y,bubbles:true,cancelable:true}));
                        window.dispatchEvent(new PointerEvent('pointermove',{pointerId:7004,buttons:1,clientX:x+24,clientY:y,bubbles:true,cancelable:true}));await frame();
                        await until(()=>+byId(id).getAttribute('aria-valuenow')===finalValue,'Horizontal knob motion differed from vertical motion');
                        key(window,'Escape','Escape');await wait(180);await frame();
                        require(source()===original,'Horizontal knob cancel did not restore source');
                        const title=target.closest('.flow-node').querySelector('.flow-node-title'); title.click(); await frame();
                        require(document.querySelector('.flow-scene.has-selection'),'Signal click did not highlight its path');
                        pointer(graph,'pointerdown',0); pointer(window,'pointerup',0); await frame();
                        require(!document.querySelector('.flow-scene.has-selection') && !document.querySelector('.flow-node.selected'),'Empty canvas did not clear selection');
                        require(!document.querySelector('.dataflow').textContent.includes('observation budget'),'Flow still repeats observation-budget messages');
                        for(const node of document.querySelectorAll('.flow-node[data-signal^="key."]')) {
                            const id=node.dataset.nodeId;
                            require([...document.querySelectorAll('.flow-edge')].some(e=>e.dataset.from===id||e.dataset.to===id),'Unused key input remains visible');
                        }
                        window.__gestureAudit.done=true;
                    })().catch(error=>{ window.__gestureAudit={done:true,error:String(error)}; });
                }
                if (action === 'pointer-drag' || action === 'pointer-down') {
                    if (!knob) throw new Error('No knob for pointer regression');
                    const rect = knob.getBoundingClientRect(), x = rect.x + rect.width / 2, y = rect.y + rect.height / 2;
                    const pointer = (target, type, offset, buttons) => target.dispatchEvent(new PointerEvent(type, {
                        pointerId: 1777, pointerType: 'mouse', isPrimary: true, button: 0, buttons,
                        clientX: x, clientY: y + offset, bubbles: true, cancelable: true
                    }));
                    pointer(knob, 'pointerdown', 0, 1);
                    if (action === 'pointer-drag') {
                        pointer(window, 'pointermove', -6, 1);
                        pointer(window, 'pointermove', -12, 1);
                        pointer(window, 'pointerup', -12, 0);
                    }
                }
                if (action === 'knob-default' || action === 'number-default' || action === 'different-numbers') {
                    // The widgets cancel pointerdown, which suppresses the browser's
                    // own dblclick, so both detect the double-click from two quick
                    // presses. Dispatch exactly what that detector waits for.
                    const numbers = [...editor.querySelectorAll('.draggable-number')];
                    const target = action === 'knob-default' ? knob :
                        (selectedKnobId ? numbers.find(element => element.dataset.parameterId === selectedKnobId) : numbers[0]);
                    if (!target) throw new Error(`No target for ${action}: wanted ${selectedKnobId}, knobs are ${knobs.map(element => `${element.dataset.parameterId}=${element.getAttribute('aria-valuenow')}`).join(' ')}`);
                    const click = element => {
                      element.scrollIntoView({block: 'nearest'});
                      const rect = element.getBoundingClientRect(), x = rect.x + rect.width / 2, y = rect.y + rect.height / 2;
                      const press = (receiver, type, buttons) => receiver.dispatchEvent(new PointerEvent(type, {
                        pointerId: 2081, pointerType: 'mouse', isPrimary: true, button: 0, buttons,
                        clientX: x, clientY: y, bubbles: true, cancelable: true
                      }));
                      press(element, 'pointerdown', 1); press(window, 'pointerup', 0);
                    };
                    click(target);
                    if (action === 'different-numbers') {
                        const other = numbers.find(element => element.dataset.parameterId !== target.dataset.parameterId);
                        if (!other) throw new Error('Cross-parameter click test needs two different numbers');
                        click(other);
                    } else click(target);
                }
                const nodes = [...document.querySelectorAll('.flow-node')].map(element => element.dataset.nodeId);
                const edges = [...document.querySelectorAll('.flow-edge')].map(element => ({
                    from: element.dataset.from, to: element.dataset.to, path: element.getAttribute('d')
                }));
                const forbidden = [...(graph?.closest('.dataflow')?.querySelectorAll('button,[role="button"]') || [])].some(element =>
                    /^(add|delete|remove)[ -](node|module|connection|edge)$|^(connect|disconnect)$/i.test(element.dataset.graphAction || '') ||
                    /^(add|delete|remove) (node|module|connection|edge)\b|^(connect|disconnect) (nodes?|modules?)\b/i.test(element.getAttribute('aria-label') || ''));
                const pixels = canvas => {
                    if (!canvas || !canvas.width || !canvas.height) return {trace: 0, hash: ''};
                    const values = canvas.getContext('2d').getImageData(0,0,canvas.width,canvas.height).data;
                    let hash = 2166136261, trace = 0;
                    // Count the actual mint trace, excluding its grey baseline.
                    for (let i=0;i<values.length;i+=16) {
                        if (values[i+3]>24 && values[i+1]>values[i]+20) ++trace;
                        for (let channel=0;channel<4;++channel) hash=Math.imul(hash^values[i+channel],16777619);
                    }
                    return {trace, hash: `${canvas.width}x${canvas.height}:${hash>>>0}`};
                };
                const rawCapsule = [...(editor?.querySelectorAll('.signal-capsule') || [])].find(element => element.querySelector('span')?.textContent === 'raw');
                return JSON.stringify({
                    interfaceFonts: ['.file-entry', '.panel-heading > span', '.compile-button', '.flow-node-title strong', '.agent-panel', '.keyboard-strip'].map(selector => {
                        const element = document.querySelector(selector);
                        return element ? parseFloat(getComputedStyle(element).fontSize) : 0;
                    }),
                    font: editor ? parseFloat(getComputedStyle(editor).fontSize) : 0,
                    // Inline signal capsules are widgets inside .cm-line. Their box
                    // must not enlarge the line box, or the editor's line pitch
                    // becomes uneven wherever a capsule appears.
                    lineHeights: [...(editor?.querySelectorAll('.cm-line') || [])].map(element => +element.getBoundingClientRect().height.toFixed(2)),
                    // This is the DOM->Tile->EditorView path used by the installed
                    // CodeMirror EditorView.findFromDOM, reading the complete doc
                    // rather than virtualized .cm-line text or App's source props.
                    editorText: editor?.cmTile?.root?.view?.state?.doc?.toString() ?? null,
                    activeFile: document.querySelector('.file-entry.active')?.title ?? '',
                    tab: (document.querySelector('.editor-group.active .editor-tab.active')?.dataset.documentId || '').split('/').pop() || document.querySelector('.document-tab > span')?.textContent?.trim() || '',
                    editorGroups: groups.map(group => ({
                        documentId: group.querySelector('.editor-tab.active')?.dataset.documentId || '',
                        active: group.classList.contains('active'),
                        text: visibleEditor(group)?.cmTile?.root?.view?.state?.doc?.toString() ?? '',
                        tabs: [...group.querySelectorAll('.editor-tab')].map(tab=>tab.dataset.documentId)
                    })),
                    closeDialog: !!document.querySelector('[role="dialog"][aria-label="Unsaved instrument"]'),
                    focusedEditorGroup: document.activeElement?.closest('[data-editor-group]')?.dataset.editorGroup ?? '',
                    banner: document.querySelector('.error-banner')?.textContent?.trim() ?? '',
                    graphReady: !!graph && nodes.length > 0 && edges.length > 0,
                    endpointsValid: new Set(nodes).size === nodes.length && nodes.every(Boolean) &&
                        edges.every(edge => nodes.includes(edge.from) && nodes.includes(edge.to) && !!edge.path),
                    signature: JSON.stringify({nodes: [...nodes].sort(), edges: edges.map(edge => `${edge.from}>${edge.to}`).sort()}),
                    gestureAudit: window.__gestureAudit || {}, displayAudit: window.__displayAudit || {}, editorAudit: window.__editorAudit || {},
                    forbidden, knobId: knob?.dataset.parameterId, knobCount: knobs.length, knobValue: knob ? +knob.getAttribute('aria-valuenow') : null,
                    explorerWidth: document.querySelector('.files-panel')?.getBoundingClientRect().width || 0,
                    separator: !!separator, dragging: document.body.classList.contains('flow-knob-dragging'),
                    stoppedPreview: phase>=112 && phase<=116 ? {
                        pitchCapsule: pixels([...(editor?.querySelectorAll('.signal-capsule') || [])].find(element=>element.querySelector('span')?.textContent==='key.pitch')?.querySelector('canvas')),
                        pitchFlow: pixels(document.querySelector('.flow-node[data-signal="key.pitch"] .flow-scope canvas')),
                        traces: [...document.querySelectorAll('.flow-scope canvas,.signal-capsule canvas,.signal-view canvas')].filter(canvas=>canvas.getClientRects().length).reduce((sum,canvas)=>sum+pixels(canvas).trace,0),
                        idle: document.querySelectorAll('.signal-capsule[data-capture-state="idle"]').length,
                        liveCapsules: document.querySelectorAll('.signal-capsule[data-capture-state="live"]').length,
                        liveFlow: document.querySelectorAll('.flow-scope[data-capture-state="live"]').length,
                        // Normal canvas opacity is .85/.88; held previews use .5/.45.
                        dimmedLive: [...document.querySelectorAll('.signal-capsule[data-capture-state="live"] canvas,.flow-scope[data-capture-state="live"] canvas')].some(canvas=>+getComputedStyle(canvas).opacity<.8),
                        heldCapsules: document.querySelectorAll('.signal-capsule[data-capture-state="held"]').length,
                        heldFlow: document.querySelectorAll('.flow-scope[data-capture-state="held"]').length,
                        lastKey: [...document.querySelectorAll('.capsule-capture-status,.flow-scope>span')].some(e=>e.textContent==='LAST KEY'&&!e.hidden)
                    } : {},
                    rawTrace: pixels(rawCapsule?.querySelector('canvas')),
                    probeCapsules: [...(editor?.querySelectorAll('.signal-capsule') || [])].map(element => element.querySelector('span')?.textContent),
                    choiceMarks: [...(editor?.querySelectorAll('.selectable-option') || [])].map(element=>element.dataset.choiceId),
                    choiceEditorAnchor: choiceAnchorState(),
                    choiceMenu: !!document.querySelector('.choice-menu'),
                    choiceMenuFocused: document.activeElement === document.querySelector('.choice-menu'),
                    readyChoices: [...document.querySelectorAll('.flow-node .choice-select-button:not([disabled])')].map(element=>element.dataset.choiceId),
                    choiceOptions: [...document.querySelectorAll('.choice-menu [role="option"]')].map(element=>element.dataset.value),
                    choiceMenuFits: (() => { const r=document.querySelector('.choice-menu')?.getBoundingClientRect(); return !!r && r.left>=0 && r.top>=0 && r.right<=innerWidth && r.bottom<=innerHeight; })(),
                    choiceMenuBounds: (() => { const r=document.querySelector('.choice-menu')?.getBoundingClientRect(); return r ? [r.left,r.top,r.right,r.bottom,innerWidth,innerHeight] : []; })(),
                    flowChoiceValue: [...document.querySelectorAll('.flow-node .choice-select-button')].find(element=>element.dataset.choiceId===selectedKnobId)?.querySelector('span')?.textContent || '',
                    inspectorOpen: !!document.querySelector('.signal-modal'),
                    inspectorName: document.querySelector('.signal-modal .signal-view header strong')?.textContent ?? '',
                    inspectorError: document.querySelector('.signal-modal .capture-error')?.textContent ?? '',
                    inspectorScalar: document.querySelector('.signal-modal .scalar-value strong')?.textContent ?? '',
                    inspectorProjection: document.querySelector('.signal-modal select[aria-label="Observation channel"]')?.value ?? '',
                    tempo: document.querySelector('input[aria-label="Tempo BPM"]')?.value ?? '',
                    inspectorVoice: document.querySelector('.signal-modal select[aria-label="Observation voice"]')?.value ?? '',
                    inspectorSpectrumReady: pixels(document.querySelector('.signal-modal [data-view-mode=spectrum] canvas')).trace > 10,
                    inspectorSpectrumSelected: [...document.querySelectorAll('.signal-modal .signal-plot')].map(p=>p.dataset.viewMode).join(',') === 'wave,spectrum',
                    inspectorListenReady: !!document.querySelector('.signal-modal [aria-label="Listen to p_after"]:not([disabled])'),
                    inspectorTrace: pixels(document.querySelector('.signal-modal .wave-container canvas')),
                    canHold: !!document.querySelector('button[aria-label="Hold observation"]'),
                    canResume: !!document.querySelector('button[aria-label="Resume observation"]'),
                    computerEnabled: !!document.querySelector('.typing-toggle.selected'),
                    panelVisibility: ['explorer-panel','assistant-panel','dataflow-panel','keyboard-strip'].map(id => !!document.getElementById(id)?.getClientRects().length),
                    visibleResizers: [...document.querySelectorAll('.resize-handle')].filter(element => element.getClientRects().length).length,
                    editorWidth: document.querySelector('.editor-splits')?.getBoundingClientRect().width || 0,
                    editorHeight: document.querySelector('.editor-splits')?.getBoundingClientRect().height || 0,
                    assistantWidth: document.querySelector('#assistant-panel')?.getBoundingClientRect().width || 0,
                    flowHeight: document.querySelector('#dataflow-panel')?.getBoundingClientRect().height || 0,
                    layoutNodesRetained: !window.__flowsLayoutNodes || ['explorer-panel','assistant-panel','dataflow-panel'].every((id,index) => document.getElementById(id) === window.__flowsLayoutNodes[index]),
                    aiConfigurationOpen: !!document.querySelector('.ai-configuration'),
                    assistantMarkdown: (() => {
                        const body=document.querySelector('.assistant-message .markdown-body');
                        return {
                            ready: !!body?.querySelector('h2') && body.querySelector('h2').textContent === 'Signal flow',
                            bold: body?.querySelector('strong')?.textContent === 'One oscillator',
                            table: !!body?.querySelector('table td'),
                            code: body?.querySelector('pre code')?.textContent || '',
                            copy: !!body?.querySelector('button[title="Copy code"]'),
                            unsafe: !!window.__unsafeMarkdown || !!body?.querySelector('script')
                        };
                    })(),
                    assistantActivity: window.__assistantActivityAudit || {},
                    assistantModels: {
                        names: [...document.querySelectorAll('select[aria-label="Assistant model"] option')].map(o=>o.textContent),
                        selected: document.querySelector('select[aria-label="Assistant model"]')?.value || '',
                        reference: document.querySelector('select[aria-label="Instrument reference"]')?.value ?? 'missing',
                        global: document.querySelector('.ai-configuration .section-title')?.textContent === 'Global models',
                        savedCount: document.querySelector('select[aria-label="Saved models"]')?.options.length || 0
                    },
                    assistantHistory: {
                        open: !!document.querySelector('.agent-history'),
                        rename: !!document.querySelector('input[aria-label="Conversation title"]'),
                        renameValue: document.querySelector('input[aria-label="Conversation title"]')?.value || '',
                        folder: document.querySelector('.agent-history-tabs button[aria-selected="true"]')?.textContent || '',
                        title: document.querySelector('[data-conversation-id="markdown-window-conversation"] .agent-history-open strong')?.textContent || '',
                        readonly: !!document.querySelector('.agent-readonly')
                    },
                    samplePlayback: document.querySelector('select[aria-label="Sample playback"]')?.value || '',
                    sampleGateDisabled: document.querySelector('input[aria-label="Selected step gate"]')?.disabled || false,
                    sequencerSource: document.querySelector('select[aria-label="Lane 1 sound source"]')?.value || '',
                    sequencerSourceOptions: [...document.querySelectorAll('select[aria-label="Lane 1 sound source"] option')].map(option=>option.value),
                    layoutShortcuts: window.__flowsLayoutShortcuts ?? [],
                    keyboardVisible: !!document.querySelector('#keyboard-strip')?.getClientRects().length,
                    computerKeys: window.__flowsKeyRegression ?? null,
                    switchedKey: window.__flowsSwitchKey ?? null
                });
            } catch (error) { return JSON.stringify({error: String(error)}); }
        }))JS") + "(" + JSON::toString(var(action), true) + "," + JSON::toString(var(knobId_), true) + "," + String(interactionStep_) + ")";
        evaluating_ = true;
        const std::weak_ptr<int> token = alive_;
        auto* owner = this;
        [window_->webView() evaluateJavaScript:[NSString stringWithUTF8String:script.toRawUTF8()]
            completionHandler:^(id result, NSError* error) {
                if (token.expired()) return;
                owner->interactionResult(result, error);
            }];
    }

    void interactionResult(id result, NSError* error) {
        evaluating_ = false;
        if (finished_) return;
        try {
            if (error != nil) throw std::runtime_error("UI regression script: " + std::string([error.localizedDescription UTF8String]));
            check([result isKindOfClass:[NSString class]], "UI regression returned no observation.");
            const auto observation = String::fromUTF8([static_cast<NSString*>(result) UTF8String]);
            lastPage_ = "interaction step " + String(interactionStep_) + ": " + observation;
            const auto state = JSON::parse(observation);
            if (str(state, "error").isNotEmpty()) throw std::runtime_error("UI phase "+std::to_string(interactionStep_)+": "+str(state, "error").toStdString());
            const auto snapshot = processor_->snapshot();
            const auto prefs = get(snapshot, "preferences"), document = get(snapshot, "document"), compile = get(snapshot, "compile");
            const bool compiled = str(compile, "status") == "ready" && get(compile, "revision") == get(document, "revision");
            const auto font = static_cast<double>(get(state, "font"));
            const auto source = str(document, "text");
            if (str(state, "banner").isNotEmpty())
                throw std::runtime_error("UI displayed an error: " + str(state, "banner").toStdString());
            const bool uiConsistent = get(state, "editorText").isString() && str(state, "editorText") == source &&
                str(state, "activeFile") == str(document, "path") &&
                str(state, "tab") == str(document, "path").fromLastOccurrenceOf("/", false, false);
            lastPage_ += "; native font=" + get(prefs, "fontSize").toString() +
                ", leftWidth=" + get(prefs, "leftWidth").toString() + ", compile=" + str(compile, "status") +
                ", sourceRevision=" + get(document, "revision").toString();
            const auto checkLiteralEdit = [&] {
                const std::u16string before(reinterpret_cast<const char16_t*>(originalSource_.toUTF16().getAddress()));
                const std::u16string after(reinterpret_cast<const char16_t*>(source.toUTF16().getAddress()));
                check(static_cast<std::size_t>(knobTo_) <= before.size(), "Knob source span exceeds its document.");
                const auto prefix = before.substr(0, static_cast<std::size_t>(knobFrom_));
                const auto suffix = before.substr(static_cast<std::size_t>(knobTo_));
                check(after.size() >= prefix.size() + suffix.size() && after.starts_with(prefix) && after.ends_with(suffix),
                      "Graph knob edited source outside its registered literal span.");
            };
            const bool knobRestored = source == originalSource_ && compiled && uiConsistent && str(state, "knobId") == knobId_ &&
                static_cast<double>(get(state, "knobValue")) == knobValue_;
            const auto capture = processor_->telemetry();
            const auto rawTrace = get(state,"rawTrace"), inspectorTrace = get(state,"inspectorTrace");
            const bool rawVisible = static_cast<int>(get(rawTrace,"trace")) > 10;
            const bool inspectorVisible = static_cast<int>(get(inspectorTrace,"trace")) > 10;
            const auto epoch = static_cast<juce::int64>(get(capture,"observationEpoch"));
            const auto resets = static_cast<juce::int64>(get(capture,"captureResets"));
            const auto captureEnd = static_cast<juce::int64>(get(capture,"captureEnd"));
            bool rawValid = false;
            if (auto values=get(capture,"signals");auto* list=values.getArray())
                for(const auto& value:*list) if(str(value,"name")=="raw") rawValid=static_cast<bool>(get(value,"valid")) && get(value,"values").isArray() && !get(value,"values").getArray()->isEmpty();
            const auto choiceReady = [&](const String& id) { const auto choices=get(state,"readyChoices");return choices.isArray() && choices.getArray()->contains(var(id)); };
            const auto choiceAnchorReady = [&] {
                const auto anchor = get(state, "choiceEditorAnchor");
                if (!static_cast<bool>(get(anchor, "visible"))) { choiceAnchorGeometry_.clear(); return false; }
                const auto geometry = JSON::toString(get(anchor, "geometry"), true);
                const bool stable = geometry == choiceAnchorGeometry_;
                choiceAnchorGeometry_ = geometry;
                return stable;
            };
            const auto checkContinuousCapture = [&] {
                check(rawVisible && rawValid,"An already-ready raw capsule lost its rendered trace or valid native capture.");
                check(epoch==observationEpoch_ && resets==captureResets_,"Opening/closing the same Inspector reset an unchanged observation set.");
            };
            switch (interactionStep_) {
                case 0: {
                    if (!compiled || !uiConsistent || !static_cast<bool>(get(state, "graphReady")) || str(state, "knobId").isEmpty()) return;
                    check(font == 14, "A fresh workspace must use 14px editor text.");
                    // A code editor's line pitch has to be uniform whether or not a
                    // line carries signal capsules, as in VSCode.
                    const auto heights = get(state, "lineHeights");
                    check(heights.isArray() && heights.getArray()->size() >= 5, "Editor rendered too few lines to measure line pitch.");
                    double shortest = std::numeric_limits<double>::max(), tallest = 0;
                    for (const auto& height : *heights.getArray()) {
                        shortest = std::min(shortest, static_cast<double>(height));
                        tallest = std::max(tallest, static_cast<double>(height));
                    }
                    check(tallest - shortest <= 0.5, ("Editor line height is not uniform: " + String(shortest, 2) + ".." + String(tallest, 2) + " px.").toRawUTF8());
                    std::cout << "Editor line pitch: " << heights.getArray()->size() << " lines, "
                              << String(shortest, 2) << ".." << String(tallest, 2) << " px\n";
                    check(static_cast<bool>(get(state, "endpointsValid")), "Dataflow edges must have rendered paths and refer to unique, existing nodes.");
                    check(!static_cast<bool>(get(state, "forbidden")), "Read-only dataflow exposes graph topology editing controls.");
                    check(static_cast<bool>(get(state, "separator")), "Explorer resize separator is missing.");
                    graphSignature_ = str(state, "signature");
                    interfaceFonts_ = get(state, "interfaceFonts").clone();
                    interactionAction_ = "font-up";
                    ++interactionStep_;
                    break;
                }
                case 1:
                    if (std::abs(font - 14.0 * 17.0 / 16.0) > 0.02 || static_cast<int>(get(prefs, "fontSize")) != 17) return;
                    checkInterfaceFonts(state, 17.0 / 16.0);
                    interactionAction_ = "font-down";
                    ++interactionStep_;
                    break;
                case 2: {
                    if (font != 14 || static_cast<int>(get(prefs, "fontSize")) != 16 || !compiled) return;
                    checkInterfaceFonts(state, 1.0);
                    originalSource_ = source;
                    originalPath_ = str(document, "path");
                    knobId_ = str(state, "knobId");
                    knobValue_ = static_cast<double>(get(state, "knobValue"));
                    const auto parameters = get(compile, "parameters");
                    bool found = false;
                    if (auto* list = parameters.getArray()) for (const auto& parameter : *list) {
                        if (str(parameter, "id") != knobId_) continue;
                        knobFrom_ = static_cast<int>(get(parameter, "from"));
                        knobTo_ = static_cast<int>(get(parameter, "to"));
                        found = true;
                        break;
                    }
                    check(found && knobFrom_ >= 0 && knobTo_ > knobFrom_, "Graph knob lacks its source parameter binding.");
                    interactionAction_ = "knob-up";
                    ++interactionStep_;
                    break;
                }
                case 3: {
                    if (source == originalSource_ || !compiled || !uiConsistent || str(state, "knobId") != knobId_ ||
                        static_cast<double>(get(state, "knobValue")) == knobValue_) return;
                    checkLiteralEdit();
                    interactionAction_ = "undo";
                    ++interactionStep_;
                    break;
                }
                case 4:
                    if (!knobRestored) return;
                    interactionAction_ = "pointer-drag";
                    ++interactionStep_;
                    break;
                case 5:
                    if (source == originalSource_ || !compiled || !uiConsistent || str(state, "knobId") != knobId_ ||
                        static_cast<double>(get(state, "knobValue")) == knobValue_) return;
                    check(!static_cast<bool>(get(state, "dragging")), "Pointer-up left the knob gesture active.");
                    checkLiteralEdit();
                    interactionAction_ = "undo";
                    ++interactionStep_;
                    break;
                case 6:
                    if (!knobRestored) return;
                    check(str(state, "signature") == graphSignature_, "Parameter edit/undo changed the graph topology.");
                    interactionAction_ = "gesture-audit"; interactionStep_ = 200; break;
                case 200: {
                    const auto audit=get(state,"gestureAudit");
                    if(!static_cast<bool>(get(audit,"done")))return;
                    if(str(audit,"error").isNotEmpty()) throw std::runtime_error("Gesture audit: "+str(audit,"error").toStdString());
                    if(!compiled || !uiConsistent)return;
                    std::cout << "Gesture UI: continuous drag, no release snapback, source sync, one-step undo, Shift/Escape, empty-canvas deselection and unused key filtering passed.\n";
                    explorerWidth_ = static_cast<double>(get(state, "explorerWidth"));
                    check(explorerWidth_ > 0, "Explorer panel has no rendered width.");
                    interactionAction_ = "resize"; interactionStep_ = 7; break;
                }
                case 7: {
                    const auto width = static_cast<double>(get(state, "explorerWidth"));
                    if (std::abs(width - explorerWidth_ - 10) > 1 || std::abs(static_cast<double>(get(prefs, "leftWidth")) - width) > 1) return;
                    const auto saved = JSON::parse(workspace_.getChildFile(".flows-settings.json").loadFileAsString());
                    const auto savedPrefs = get(saved, "preferences");
                    if (std::abs(static_cast<double>(get(savedPrefs, "leftWidth")) - width) > 1 ||
                        static_cast<int>(get(savedPrefs, "fontSize")) != 16) return;
                    check(font == 14 && static_cast<bool>(get(state, "endpointsValid")) && !static_cast<bool>(get(state, "forbidden")),
                          "Panel resizing disturbed editor font or graph integrity.");
                    interactionAction_ = "pointer-down";
                    ++interactionStep_;
                    break;
                }
                case 8:
                    if (!static_cast<bool>(get(state, "dragging"))) return;
                    switchedSource_ = workspace_.getChildFile("examples/sine-reference.flows").loadFileAsString();
                    check(switchedSource_.isNotEmpty(), "Missing isolated sine fixture.");
                    processor_->request("document.open", object({{"path", "examples/sine-reference.flows"}}));
                    knobId_.clear();
                    ++interactionStep_;
                    break;
                case 9:
                    check(str(document, "path") == "examples/sine-reference.flows" && source == switchedSource_,
                          "Unmounting an active knob copied the previous instrument into the newly opened document.");
                    if (!compiled || !uiConsistent || !static_cast<bool>(get(state, "graphReady")) ||
                        str(state, "signature") == graphSignature_ || static_cast<bool>(get(state, "dragging"))) return;
                    if (documentReadyAt_ == Clock::time_point{}) documentReadyAt_ = Clock::now();
                    // A stale unmount callback may enqueue document.apply with the
                    // UI's 25 ms debounce. Observe beyond it and a state publication.
                    if (Clock::now() - documentReadyAt_ < std::chrono::milliseconds(400)) return;
                    interactionAction_ = "open:" + originalPath_;
                    documentReadyAt_ = {};
                    ++interactionStep_;
                    break;
                case 10: {
                    if (!compiled || !uiConsistent || source != originalSource_ || str(document, "path") != originalPath_ ||
                        str(state, "signature") != graphSignature_ || static_cast<bool>(get(state, "dragging"))) return;
                    if (documentReadyAt_ == Clock::time_point{}) documentReadyAt_ = Clock::now();
                    if (Clock::now() - documentReadyAt_ < std::chrono::milliseconds(400)) return;
                    std::cout << "UI interactions: font 16 -> 17 -> 16; graph endpoints/read-only controls; keyboard and pointer knob edits each undo once; explorer +10px and persisted preferences; active knob unmount preserves the next document.\n";
                    processor_->request("document.open",object({{"path","examples/sine-reference.flows"}}));
                    processor_->prepareToPlay(48000,128);pumpingAudio_=true;
                    // Continuous inspection now requires transport; a muted lane
                    // preserves the single held MIDI voice used by these checks.
                    auto sequence=get(processor_->snapshot(),"sequencer").clone();
                    for(auto& row:*get(sequence,"rows").getArray())row.getDynamicObject()->setProperty("mute",true);
                    processor_->request("sequencer.set",object({{"baseRevision",get(sequence,"revision")},{"pattern",sequence}}));
                    processor_->request("sequencer.play",object({{"playing",true}}));
                    observationReadyAt_={};++interactionStep_;
                    break;
                }
                case 11:
                    if (!compiled || !uiConsistent || str(document,"path")!="examples/sine-reference.flows" || !noteSent_ || !rawVisible || !rawValid || captureEnd<=0) { observationReadyAt_={}; return; }
                    check(capture.hasProperty("observationEpoch") && capture.hasProperty("captureResets"),"Native telemetry lacks observation continuity counters.");
                    // Let the new document's graph/visible-capsule selection
                    // settle before asserting continuity of the chosen set.
                    if (observationReadyAt_==Clock::time_point{} || observationEpoch_!=epoch || captureResets_!=resets) {
                        observationEpoch_=epoch;captureResets_=resets;observationReadyAt_=Clock::now();return;
                    }
                    if (Clock::now()-observationReadyAt_<std::chrono::milliseconds(200)) return;
                    observationReadyAt_=Clock::now();++interactionStep_;
                    break;
                case 12:
                    checkContinuousCapture();
                    if (Clock::now()-observationReadyAt_<std::chrono::milliseconds(500)) return;
                    interactionAction_="inspect-raw";observationReadyAt_={};++interactionStep_;
                    break;
                case 13:
                    checkContinuousCapture();
                    if (!static_cast<bool>(get(state,"inspectorOpen")) || !inspectorVisible) return;
                    if (observationReadyAt_==Clock::time_point{}) observationReadyAt_=Clock::now();
                    if (Clock::now()-observationReadyAt_<std::chrono::milliseconds(250)) return;
                    interactionAction_="hold-observation";observationReadyAt_={};++interactionStep_;
                    break;
                case 14:
                    checkContinuousCapture();
                    if (!static_cast<bool>(get(state,"canResume"))) return;
                    check(inspectorVisible,"Hold cleared the Inspector trace.");
                    if (!heldPixelsRecorded_) {
                        heldPixelsRecorded_=true;heldCapsuleHash_=str(rawTrace,"hash");heldInspectorHash_=str(inspectorTrace,"hash");
                        heldCaptureEnd_=captureEnd;heldSequence_=static_cast<juce::int64>(get(capture,"sequence"));heldRenderedSamples_=renderedSamples_;observationReadyAt_=Clock::now();
                    }
                    check(str(rawTrace,"hash")==heldCapsuleHash_ && str(inspectorTrace,"hash")==heldInspectorHash_,"Shared Hold did not freeze both capsule and Inspector canvas pixels.");
                    if (Clock::now()-observationReadyAt_<std::chrono::milliseconds(500)) return;
                    check(captureEnd>heldCaptureEnd_ && static_cast<juce::int64>(get(capture,"sequence"))>heldSequence_ && renderedSamples_>heldRenderedSamples_ && static_cast<double>(get(capture,"outputPeak"))>.05 && static_cast<int>(get(capture,"voices"))==1,
                          "Hold stopped audio processing or native telemetry instead of only freezing observation.");
                    interactionAction_="resume-observation";++interactionStep_;
                    break;
                case 15:
                    checkContinuousCapture();
                    if (!static_cast<bool>(get(state,"canHold")) || !inspectorVisible || (str(rawTrace,"hash")==heldCapsuleHash_ && str(inspectorTrace,"hash")==heldInspectorHash_)) return;
                    interactionAction_="close-inspector";observationReadyAt_={};++interactionStep_;
                    break;
                case 16:
                    checkContinuousCapture();
                    if (static_cast<bool>(get(state,"inspectorOpen"))) return;
                    if (observationReadyAt_==Clock::time_point{}) observationReadyAt_=Clock::now();
                    if (Clock::now()-observationReadyAt_<std::chrono::milliseconds(500)) return;
                    std::cout << "Capture UI: actual held MIDI/processBlock capture; raw trace continuously present for 500ms; same-set Inspector open/close preserves observation epoch and resets; shared Hold freezes both canvases for 500ms while audio/capture advance; Resume redraws.\n";
                    interactionAction_="display-audit"; interactionStep_=201; break;
                case 201: {
                    const auto audit=get(state,"displayAudit");
                    if(!static_cast<bool>(get(audit,"done")))return;
                    if(str(audit,"error").isNotEmpty())throw std::runtime_error("Display audit: "+str(audit,"error").toStdString());
                    check(source==switchedSource_,"Display choices changed the instrument source.");
                    std::cout << "Signal display UI: audio defaults to spectrum; preview menu switches both surfaces; Inspector stacks waveform above spectrum without source edits.\n";
                    processor_->request("keyboard.panic",object({}));
                    interactionAction_="computer-enable";interactionStep_=17;break;
                }
                case 17:
                    if (!static_cast<bool>(get(state,"computerEnabled")) || static_cast<int>(get(get(snapshot,"audio"),"voices"))!=0) return;
                    interactionAction_="computer-hide";interactionStep_=171;
                    break;
                case 171:
                    if (static_cast<bool>(get(state,"keyboardVisible"))) return;
                    check(static_cast<bool>(get(state,"computerEnabled")),"Hiding the piano disabled computer-key mode.");
                    interactionAction_="computer-hold";interactionStep_=18;
                    break;
                case 18: {
                    const auto keys=get(state,"computerKeys");
                    if (!keys.hasProperty("initial")) return;
                    check(static_cast<bool>(get(keys,"readonly"))&&static_cast<bool>(get(keys,"editBlocked")),"Perform mode did not lock source edits.");
                    check(static_cast<bool>(get(keys,"initial")),"Mapped computer keydown was not consumed in the locked editor.");
                    const auto repeats=get(keys,"repeats"),editing=get(keys,"editing");
                    check(repeats.isArray()&&repeats.getArray()->size()==30,"Computer key repeat regression did not dispatch its full sequence.");
                    for (const auto& consumed:*repeats.getArray()) check(static_cast<bool>(consumed),"A held computer-key repeat was not prevented; macOS can beep.");
                    check(editing.isArray()&&editing.getArray()->size()==2&&!static_cast<bool>((*editing.getArray())[0])&&!static_cast<bool>((*editing.getArray())[1]),"Computer keys intercepted ordinary editor typing or its repeat.");
                    check(!static_cast<bool>(get(keys,"shortcut")),"Computer keys intercepted a mapped Command shortcut.");
                    check(static_cast<int>(get(keys,"on"))==1&&static_cast<int>(get(keys,"off"))==0,"Held repeats or excluded editing/shortcut events sent extra native note requests.");
                    check(source==switchedSource_,"Synthetic keyboard regression unexpectedly edited the instrument.");
                    if (static_cast<int>(get(get(snapshot,"audio"),"voices"))!=1) return;
                    interactionAction_="computer-show";interactionStep_=181;
                    break;
                }
                case 181:
                    if (!static_cast<bool>(get(state,"keyboardVisible"))) return;
                    check(static_cast<int>(get(get(state,"computerKeys"),"off"))==0,"Showing the piano interrupted a held computer note.");
                    interactionAction_="computer-hide";interactionStep_=182;
                    break;
                case 182:
                    if (static_cast<bool>(get(state,"keyboardVisible"))) return;
                    check(static_cast<int>(get(get(state,"computerKeys"),"off"))==0,"Hiding the piano interrupted a held computer note.");
                    interactionAction_="computer-release";interactionStep_=19;
                    break;
                case 19: {
                    const auto keys=get(state,"computerKeys");
                    if (!keys.hasProperty("released")) return;
                    check(static_cast<bool>(get(keys,"released")),"Held computer-key keyup was not consumed.");
                    check(static_cast<int>(get(keys,"on"))==1&&static_cast<int>(get(keys,"off"))==1,"Computer key release must send exactly one native note-off.");
                    if (static_cast<int>(get(get(snapshot,"audio"),"voices"))!=0) return;
                    interactionAction_="octave-audit";interactionStep_=191;
                    break;
                }
                case 191: {
                    const auto keys=get(state,"computerKeys");
                    if (!static_cast<bool>(get(keys,"octaveDone"))) return;
                    check(str(keys,"octaveError").isEmpty(),str(keys,"octaveError").toRawUTF8());
                    check(source==switchedSource_,"Octave shortcuts changed instrument source.");
                    std::cout << "Octave shortcuts: Z/X transpose next native notes by 12 semitones, release held notes, repeat within C0..C6, work with a hidden piano, preserve keyup ownership and exclude text, modifiers, IME and Edit mode.\n";
                    interactionAction_="computer-cleanup";interactionStep_=20;break;
                }
                case 20: {
                    if (static_cast<bool>(get(state,"computerEnabled")) || !static_cast<bool>(get(state,"keyboardVisible"))) return;
                    check(static_cast<int>(get(get(snapshot,"audio"),"voices"))==0,"Computer key release left a stuck native voice.");
                    std::cout << "Computer keyboard: hidden piano still consumes initial + 30 repeats + keyup; showing/hiding preserves held notes; exactly one native note-on/off; Perform locks source and plays from the focused editor; text-field typing and Command shortcuts remain available.\n";
                    // A class declares what each port falls back to. Pick a port
                    // that currently sits away from its default and drive both
                    // gestures, because each reaches the same native parameter
                    // through a different UI path.
                    const auto parameters=get(compile,"parameters");
                    if (auto* list=parameters.getArray()) for (const auto& parameter:*list) {
                        if (!static_cast<bool>(get(parameter,"hasDefault"))) continue;
                        const auto fallback=static_cast<double>(get(parameter,"defaultValue"));
                        if (std::abs(static_cast<double>(get(parameter,"value"))-fallback)<1e-9) continue;
                        resetId_=str(parameter,"id"); resetValue_=fallback; break;
                    }
                    check(resetId_.isNotEmpty(),"The open instrument exposes no parameter with a class default to restore.");
                    knobId_=resetId_;
                    beforeReset_=source;
                    interactionAction_="number-default"; ++interactionStep_;
                    break;
                }
                case 21:
                    if (!compiled || !uiConsistent || source==beforeReset_) return;
                    check(resetValueMatches(compile),"A double-click on the number must write the class default back into the literal.");
                    interactionAction_="undo"; ++interactionStep_;
                    break;
                case 22:
                    // The graph only grows knobs once the recompiled metadata
                    // lands, so the second gesture waits for them.
                    if (!compiled || !uiConsistent || source!=beforeReset_ || static_cast<int>(get(state,"knobCount"))==0) return;
                    knobId_=resetId_;
                    interactionAction_="knob-default"; ++interactionStep_;
                    break;
                case 23: {
                    if (!compiled || !uiConsistent || source==beforeReset_) return;
                    check(resetValueMatches(compile),"A double-click on the knob must write the class default back into the literal.");
                    std::cout << "Class defaults: the editor number and the signal-flow knob each restored "+resetId_+" to its class default and left an undoable edit.\n";
                    defaultRegressionSource_=R"FLOWS(import standard as std
from lib.osc import ClassicSine
from lib.filt import ClassicFilt
class Quiet(ClassicSine):
    def init:
        self.register_knob(level, default=-18.25db, range=(-60db, 0db), step=1db)
Inst.sigstream(std.event.keys.poly()) as key:
    osc = Quiet.make()
    filt = ClassicFilt.make()
    raw = osc(pitch: key.pitch, level: -3db)
    return filt(raw, cutoff: 1234hz) * key.gate
)FLOWS";
                    check(workspace_.getChildFile("default-regression.flows").replaceWithData(defaultRegressionSource_.toRawUTF8(),defaultRegressionSource_.getNumBytesAsUTF8()),"Could not create isolated default regression instrument.");
                    processor_->request("document.open",object({{"path","default-regression.flows"}}));
                    knobId_.clear(); ++interactionStep_;
                    break;
                }
                case 24: {
                    if (!compiled || !uiConsistent || str(document,"path")!="default-regression.flows" || source!=defaultRegressionSource_) return;
                    resetId_.clear(); resetValue_=-18.25;
                    const auto parameters=get(compile,"parameters");
                    if (auto* list=parameters.getArray()) for (const auto& parameter:*list)
                        if (str(parameter,"name")=="level") {
                            resetId_=str(parameter,"id");
                            check(static_cast<double>(get(parameter,"defaultValue"))==resetValue_,"Class default was lost before reaching the editor.");
                        }
                    check(resetId_.isNotEmpty(),"Class default regression has no level parameter.");
                    knobId_=resetId_; beforeReset_=source; documentReadyAt_={};
                    interactionAction_="different-numbers"; ++interactionStep_;
                    break;
                }
                case 25:
                    check(source==beforeReset_,"Single clicks on two different numbers accidentally reset a parameter.");
                    if (!compiled || !uiConsistent) return;
                    if (documentReadyAt_==Clock::time_point{}) documentReadyAt_=Clock::now();
                    if (Clock::now()-documentReadyAt_<std::chrono::milliseconds(500)) return;
                    interactionAction_="number-default"; ++interactionStep_;
                    break;
                case 26:
                    if (!compiled || !uiConsistent || source==beforeReset_) return;
                    check(resetValueMatches(compile) && source.contains("level: -18.25db"),"Source default reset quantized the declared default to the drag step.");
                    interactionAction_="undo"; ++interactionStep_;
                    break;
                case 27:
                    if (!compiled || !uiConsistent || source!=beforeReset_ || static_cast<int>(get(state,"knobCount"))==0) return;
                    interactionAction_="knob-default"; ++interactionStep_;
                    break;
                case 28:
                    if (!compiled || !uiConsistent || source==beforeReset_) return;
                    check(resetValueMatches(compile) && source.contains("level: -18.25db"),"Signal-flow reset quantized the declared default to the drag step.");
                    interactionAction_="undo"; ++interactionStep_;
                    break;
                case 29: {
                    if (!compiled || !uiConsistent || source!=beforeReset_) return;
                    std::cout << "Default regressions: rapid clicks on distinct parameters preserve source; editor and graph restore exact -18.25db despite 1db drag step; each reset undoes once.\n";
                    layoutEditorWidth_=static_cast<double>(get(state,"editorWidth")); layoutEditorHeight_=static_cast<double>(get(state,"editorHeight"));
                    explorerWidth_=static_cast<double>(get(state,"explorerWidth")); layoutRightWidth_=static_cast<double>(get(state,"assistantWidth")); layoutFlowHeight_=static_cast<double>(get(state,"flowHeight"));
                    interactionAction_="panels-hide-all"; ++interactionStep_;
                    break;
                }
                case 30: {
                    const auto visibility=get(state,"panelVisibility");
                    if (!visibility.isArray() || visibility.getArray()->size()!=4) return;
                    for (const auto& visible:*visibility.getArray()) if (static_cast<bool>(visible)) return;
                    check(source==beforeReset_,"Hiding panels changed instrument source.");
                    check(static_cast<int>(get(state,"visibleResizers"))==0,"Hidden panels left visible resize handles.");
                    check(static_cast<double>(get(state,"editorWidth"))>layoutEditorWidth_+200 && static_cast<double>(get(state,"editorHeight"))>layoutEditorHeight_+100,"Hiding panels did not return their space to the editor.");
                    check(static_cast<bool>(get(state,"layoutNodesRetained")),"Hiding panels discarded mounted state.");
                    const auto saved=JSON::parse(workspace_.getChildFile(".flows-settings.json").loadFileAsString());
                    for (const auto* flag:{"showExplorer","showAssistant","showDataflow","showKeyboard"})
                        if (!get(prefs,flag).isBool() || static_cast<bool>(get(prefs,flag)) || !get(get(saved,"preferences"),flag).isBool() || static_cast<bool>(get(get(saved,"preferences"),flag))) return;
                    interactionAction_="panels-shortcuts"; ++interactionStep_;
                    break;
                }
                case 31: {
                    const auto visibility=get(state,"panelVisibility"), shortcuts=get(state,"layoutShortcuts");
                    if (!visibility.isArray() || visibility.getArray()->size()!=4) return;
                    for (const auto& visible:*visibility.getArray()) if (!static_cast<bool>(visible)) return;
                    check(shortcuts.isArray() && shortcuts.getArray()->size()==8,"Panel shortcut regression did not run all modifiers and repeats.");
                    for (const auto& consumed:*shortcuts.getArray()) check(static_cast<bool>(consumed),"A panel shortcut or its held repeat was not consumed.");
                    check(source==beforeReset_ && static_cast<bool>(get(state,"layoutNodesRetained")) && static_cast<bool>(get(state,"aiConfigurationOpen")),"Showing panels lost source or AI panel state.");
                    if (std::abs(static_cast<double>(get(state,"editorWidth"))-layoutEditorWidth_)>1 || std::abs(static_cast<double>(get(state,"editorHeight"))-layoutEditorHeight_)>1) return;
                    check(std::abs(static_cast<double>(get(state,"explorerWidth"))-explorerWidth_)<=1 && std::abs(static_cast<double>(get(state,"assistantWidth"))-layoutRightWidth_)<=1 && std::abs(static_cast<double>(get(state,"flowHeight"))-layoutFlowHeight_)<=1,"Showing panels lost their saved dimensions.");
                    std::cout << "Panel visibility: four header buttons/tooltips; hide frees editor space and saves flags; Cmd+B / Cmd+Option+B / Cmd+J / Cmd+Shift+J restore panels once despite repeats; source, dimensions and AI view retained.\n";
                    multiRightSource_=workspace_.getChildFile("examples/probe-lab.flows").loadFileAsString();
                    multiLeftDraft_=beforeReset_+"\n# Left group draft\n";
                    interactionAction_="multi-split"; ++interactionStep_;
                    break;
                }
                case 32: {
                    const auto groups=get(state,"editorGroups");
                    if (!groups.isArray() || groups.getArray()->size()!=2) return;
                    interactionAction_="open:examples/probe-lab.flows"; ++interactionStep_;
                    break;
                }
                case 33: {
                    if (!compiled || !uiConsistent || str(document,"id")!="examples/probe-lab.flows") return;
                    if (!static_cast<bool>(get(state,"graphReady")) || !str(state,"signature").contains("signal:filtered")) return;
                    const auto groups=get(state,"editorGroups");
                    check(groups.isArray() && groups.getArray()->size()==2,"Opening an instrument removed a split group.");
                    check(str((*groups.getArray())[0],"documentId")=="default-regression.flows" && str((*groups.getArray())[1],"documentId")=="examples/probe-lab.flows","Explorer did not open into the focused group independently.");
                    check(str((*groups.getArray())[0],"text")==beforeReset_ && source==multiRightSource_,"Split groups mixed their source buffers.");
                    interactionAction_="multi-hold-and-focus-left"; interactionStep_=331;
                    break;
                }
                case 331:
                    if (!static_cast<bool>(get(state,"computerEnabled"))) return;
                    interactionAction_="multi-key-down-and-focus-left"; interactionStep_=34;
                    break;
                case 34:
                    if (!compiled || !uiConsistent || str(document,"id")!="default-regression.flows") return;
                    interactionAction_="multi-release-after-switch"; interactionStep_=341;
                    break;
                case 341: {
                    const auto held=get(state,"switchedKey"), repeats=get(held,"repeats");
                    if (!repeats.isArray() || static_cast<bool>(get(state,"computerEnabled"))) return;
                    check(static_cast<bool>(get(held,"initial")) && static_cast<bool>(get(held,"released")),"A held performance key lost input ownership when switching instruments.");
                    for (const auto& consumed:*repeats.getArray()) check(static_cast<bool>(consumed),"A released instrument key's repeat leaked into the newly focused source editor.");
                    check(source==beforeReset_ && static_cast<int>(get(get(snapshot,"audio"),"voices"))==0,"Switching instruments left an old voice or modified the new instrument's source.");
                    interactionAction_="multi-edit-left-and-focus-right"; interactionStep_=35;
                    break;
                }
                case 35: {
                    if (!compiled || !uiConsistent || str(document,"id")!="examples/probe-lab.flows") return;
                    if (!static_cast<bool>(get(state,"graphReady")) || !str(state,"signature").contains("signal:filtered")) return;
                    const auto documents=get(snapshot,"documents"), groups=get(state,"editorGroups");
                    bool retained=false;
                    if (auto* list=documents.getArray()) for (const auto& item:*list) if (str(item,"id")=="default-regression.flows") retained=str(item,"text")==multiLeftDraft_ && static_cast<bool>(get(item,"dirty"));
                    check(retained && source==multiRightSource_,"Typing then immediately focusing another group lost or misrouted the pending edit.");
                    check(str((*groups.getArray())[0],"text")==multiLeftDraft_,"Inactive editor did not retain its own draft.");
                    interactionAction_="multi-focus-left"; ++interactionStep_;
                    break;
                }
                case 36:
                    if (!compiled || !uiConsistent || str(document,"id")!="default-regression.flows") return;
                    check(source==multiLeftDraft_,"Focus return discarded the unsaved left instrument.");
                    interactionAction_="undo"; ++interactionStep_;
                    break;
                case 37:
                    if (!compiled || !uiConsistent || source!=beforeReset_) return;
                    interactionAction_="multi-close-right-active"; ++interactionStep_;
                    break;
                case 38: {
                    check(str(document,"id")=="default-regression.flows" && source==beforeReset_,"Closing an inactive tab changed the performing instrument.");
                    const auto documents=get(snapshot,"documents"), groups=get(state,"editorGroups");
                    if (auto* list=documents.getArray()) for (const auto& item:*list) if (str(item,"id")=="examples/probe-lab.flows") return;
                    check(groups.isArray() && groups.getArray()->size()==2,"Closing a tab removed its editor group.");
                    // Native close can finish before React has selected the
                    // duplicate tab. Do not click the just-closed tab again.
                    if (str((*groups.getArray())[1],"documentId")!="default-regression.flows") return;
                    interactionAction_="multi-close-right-active"; ++interactionStep_;
                    break;
                }
                case 39: {
                    const auto groups=get(state,"editorGroups");
                    if (!groups.isArray() || str((*groups.getArray())[1],"documentId").isNotEmpty()) return;
                    check(str(document,"id")=="default-regression.flows", "Closing a duplicate tab closed the instrument still visible in the other group.");
                    interactionAction_="multi-edit-and-close"; ++interactionStep_;
                    break;
                }
                case 40:
                    if (!static_cast<bool>(get(state,"closeDialog"))) return;
                    interactionAction_="multi-cancel-close"; ++interactionStep_;
                    break;
                case 41:
                    if (static_cast<bool>(get(state,"closeDialog")) || !compiled || !uiConsistent) return;
                    check(str(document,"id")=="default-regression.flows" && source==multiLeftDraft_,"Cancel close lost an unsaved instrument.");
                    interactionAction_="multi-close-left"; ++interactionStep_;
                    break;
                case 42:
                    if (!static_cast<bool>(get(state,"closeDialog"))) return;
                    interactionAction_="multi-discard-close"; ++interactionStep_;
                    break;
                case 43: {
                    if (static_cast<bool>(get(state,"closeDialog")) || str(document,"id")=="default-regression.flows" || !compiled || !uiConsistent) return;
                    const auto documents=get(snapshot,"documents");
                    if (auto* list=documents.getArray()) for (const auto& item:*list) check(str(item,"id")!="default-regression.flows","Discard close left a stale open document.");
                    interactionAction_="multi-reopen-empty"; ++interactionStep_;
                    break;
                }
                case 44: {
                    if (!compiled || !uiConsistent || str(document,"id")!="examples/probe-lab.flows" || str(state,"focusedEditorGroup")!="1") return;
                    if (!static_cast<bool>(get(state,"graphReady")) || !str(state,"signature").contains("signal:filtered")) return;
                    const auto groups=get(state,"editorGroups");
                    check(groups.isArray() && groups.getArray()->size()==2 && str((*groups.getArray())[0],"documentId")!="examples/probe-lab.flows" && str((*groups.getArray())[1],"documentId")=="examples/probe-lab.flows","Reopening into an empty group replaced the other group's instrument.");
                    std::cout << "Multi-instrument editors: split groups open different instruments, focus selects native source/audio/graph, pending edits flush to their owner, undo stays local, inactive/duplicate tabs close safely, unsaved close supports Cancel/Discard, empty groups reopen, held performance key repeats never leak into source.\n";
                    auto sequence=get(processor_->snapshot(),"sequencer").clone();
                    for(auto& row:*get(sequence,"rows").getArray())row.getDynamicObject()->setProperty("mute",true);
                    processor_->request("sequencer.set",object({{"baseRevision",get(sequence,"revision")},{"pattern",sequence}}));
                    processor_->request("sequencer.play",object({{"playing",true}}));
                    capturePath_="examples/probe-lab.flows"; noteSent_=false;
                    interactionAction_="probe-scroll:p_after"; ++interactionStep_;
                    break;
                }
                case 45: case 49: case 51: case 54: {
                    const auto name=interactionStep_==45 ? "p_after" : interactionStep_==49 ? "power_diff" : interactionStep_==51 ? "amp_power" : "mel_diff";
                    const auto capsules=get(state,"probeCapsules");
                    if (!capsules.isArray() || !capsules.getArray()->contains(var(name))) return;
                    interactionAction_="probe-inspect:"+String(name); ++interactionStep_;
                    break;
                }
                case 46: {
                    if (str(state,"inspectorName")!="p_after" || !inspectorVisible || !static_cast<bool>(get(state,"inspectorSpectrumReady"))) return;
                    check(static_cast<bool>(get(state,"inspectorListenReady")),"Direct audio probe cannot be auditioned from Inspector.");
                    const auto selected=get(get(snapshot,"observation"),"signals");
                    check(selected.isArray() && selected.getArray()->size()>8,"UI truncated observation names instead of counting shared capture roots.");
                    interactionAction_="probe-spectrum"; ++interactionStep_;
                    break;
                }
                case 47:
                    if (!static_cast<bool>(get(state,"inspectorSpectrumSelected")) || !inspectorVisible) return;
                    interactionAction_="probe-listen"; ++interactionStep_;
                    break;
                case 48:
                    if (str(get(snapshot,"audio"),"audition")!="p_after") return;
                    check(static_cast<int>(get(get(snapshot,"audio"),"voices"))==1,"Probe inspection or audition created extra DSP voices.");
                    interactionAction_="probe-scroll:power_diff"; ++interactionStep_;
                    break;
                case 50:
                    if (str(state,"inspectorName")!="power_diff" || str(state,"inspectorScalar").isEmpty()) return;
                    check(std::isfinite(str(state,"inspectorScalar").getDoubleValue()) && str(state,"inspectorError").isEmpty(),"Power difference failed to display a finite scalar.");
                    check(str(get(snapshot,"audio"),"audition").isEmpty(),"Closing a probe Inspector left its audition active.");
                    interactionAction_="probe-scroll:amp_power"; ++interactionStep_;
                    break;
                case 52:
                    if (str(state,"inspectorName")!="amp_power" || str(state,"inspectorError").isEmpty()) return;
                    check(str(state,"inspectorVoice")=="-1" && str(state,"inspectorError").containsIgnoreCase("voice"),"Mixed poly control analysis must explain the need to choose a voice.");
                    interactionAction_="probe-voice-one"; ++interactionStep_;
                    break;
                case 53:
                    if (str(state,"inspectorVoice")!="0" || str(state,"inspectorScalar").isEmpty() || static_cast<int>(get(capture,"captureVoice"))!=0) return;
                    check(str(state,"inspectorError").isEmpty() && str(state,"inspectorScalar").getDoubleValue()>0,"Selected voice envelope analysis did not recover to measured power.");
                    interactionAction_="probe-scroll:mel_diff"; ++interactionStep_;
                    break;
                case 55:
                    if (str(state,"inspectorName")!="mel_diff" || !inspectorVisible) return;
                    check(source==multiRightSource_ && compiled && uiConsistent,"Observation operations changed the performing instrument's code.");
                    check(str(state,"inspectorError").isEmpty(),"Mel difference Inspector shows an analysis error.");
                    std::cout << "Probe Inspector: shared-root subscriptions exceed eight views; alias waveform/spectrum and real audition; finite power difference; explicit mixed-control error and single-voice recovery; rendered mel difference; no extra voices or source edits.\n";
                    interactionAction_="close-inspector"; ++interactionStep_;
                    break;
                case 56:
                    if (static_cast<bool>(get(state,"inspectorOpen"))) return;
                    interactionAction_="open:examples/stereo-motion.flows"; ++interactionStep_;
                    break;
                case 57: {
                    if (!compiled || !uiConsistent || str(document,"path")!="examples/stereo-motion.flows" || !str(state,"signature").contains("signal:shaped")) return;
                    check(static_cast<int>(get(compile,"outputChannels"))==2,"Stereo instrument lacks native channel metadata.");
                    auto sequence=get(processor_->snapshot(),"sequencer").clone();
                    for(auto& row:*get(sequence,"rows").getArray())row.getDynamicObject()->setProperty("mute",true);
                    processor_->request("sequencer.set",object({{"baseRevision",get(sequence,"revision")},{"pattern",sequence}}));
                    processor_->request("sequencer.play",object({{"playing",true}}));
                    capturePath_="examples/stereo-motion.flows"; noteSent_=false; stereoDifference_=0;
                    interactionAction_="probe-scroll:p_after"; ++interactionStep_;
                    break;
                }
                case 58: {
                    const auto capsules=get(state,"probeCapsules");
                    if (!capsules.isArray() || !capsules.getArray()->contains(var("p_after"))) return;
                    interactionAction_="probe-inspect:p_after"; ++interactionStep_;
                    break;
                }
                case 59:
                    if (str(state,"inspectorName")!="p_after" || !inspectorVisible || stereoDifference_<.001) return;
                    check(str(state,"inspectorProjection")=="mid","A new stereo instrument did not start with an explicit Mid observation.");
                    interactionAction_="tempo-edit"; ++interactionStep_;
                    break;
                case 60:
                    if (str(state,"tempo")!="120") return;
                    interactionAction_="tempo-commit"; ++interactionStep_;
                    break;
                case 61:
                    if (static_cast<double>(get(get(snapshot,"audio"),"bpm"))!=120 || str(state,"tempo")!="120") return;
                    interactionAction_="probe-channel:right"; ++interactionStep_;
                    break;
                case 62:
                    if (str(state,"inspectorProjection")!="right" || str(capture,"captureProjection")!="right" || !inspectorVisible) return;
                    check(str(state,"inspectorError").isEmpty(),"Right-channel observation failed.");
                    interactionAction_="probe-channel:side"; ++interactionStep_;
                    break;
                case 63:
                    if (str(state,"inspectorProjection")!="side" || str(capture,"captureProjection")!="side" || !inspectorVisible) return;
                    check(str(state,"inspectorError").isEmpty() && source==workspace_.getChildFile("examples/stereo-motion.flows").loadFileAsString(),"Channel/tempo controls modified stereo instrument code.");
                    std::cout << "Stereo UI: LFO/unison/distortion instrument compiled and emitted distinct L/R; standalone BPM edited to 120; Inspector selected actual Right then Side capture with no source edits.\n";
                    choiceSource_=source;
                    interactionAction_="close-inspector"; ++interactionStep_;
                    break;
                case 64:
                    if (static_cast<bool>(get(state,"inspectorOpen"))) return;
                    knobId_.clear();
                    if (auto choices=get(compile,"choices");auto* list=choices.getArray())
                        for(const auto& choice:*list) if(str(choice,"name")=="shape") knobId_=str(choice,"id");
                    check(knobId_.isNotEmpty(),"Missing compiler shape choices.");
                    choiceAnchorGeometry_.clear();
                    interactionAction_="choice-scroll"; ++interactionStep_;
                    break;
                case 65:
                    if (auto marks=get(state,"choiceMarks");!marks.isArray() || !marks.getArray()->contains(var(knobId_))) return;
                    // CodeMirror renders a span before its deferred scrollIntoView
                    // measurement finishes. Wait for a hit-testable anchor with
                    // unchanged geometry across separate timer observations, so
                    // the pending scroll cannot immediately dismiss the popup.
                    if (!choiceAnchorReady()) return;
                    interactionAction_="choice-editor"; ++interactionStep_;
                    break;
                case 66:
                    if (!static_cast<bool>(get(state,"choiceMenu")) || !static_cast<bool>(get(state,"choiceMenuFocused"))) return;
                    check(static_cast<bool>(get(state,"choiceMenuFits")) && get(state,"choiceOptions").getArray()->size()==5,("Code choice menu must fit viewport and expose all five LFO shapes: " + JSON::toString(get(state,"choiceMenuBounds")).toStdString() + "; options=" + JSON::toString(get(state,"choiceOptions")).toStdString()).c_str());
                    choiceAnchorGeometry_.clear();
                    interactionAction_="choice-escape"; ++interactionStep_;
                    break;
                case 67:
                    if (static_cast<bool>(get(state,"choiceMenu"))) return;
                    check(source==choiceSource_,"Escape changed the instrument.");
                    if (!choiceAnchorReady()) return;
                    interactionAction_="choice-editor"; ++interactionStep_;
                    break;
                case 68:
                    if (!static_cast<bool>(get(state,"choiceMenu"))) return;
                    interactionAction_="choice-pick:sample_hold"; ++interactionStep_;
                    break;
                case 69:
                    if (!compiled || !uiConsistent || str(state,"flowChoiceValue")!="sample_hold") return;
                    check(source==choiceSource_.replace("shape: triangle","shape: sample_hold"),"Code dropdown changed more than the selected value.");
                    interactionAction_="undo"; ++interactionStep_;
                    break;
                case 70:
                    if (!compiled || !uiConsistent || source!=choiceSource_ || str(state,"flowChoiceValue")!="triangle" || !choiceReady(knobId_)) return;
                    interactionAction_="choice-flow"; ++interactionStep_;
                    break;
                case 71:
                    if (!static_cast<bool>(get(state,"choiceMenu")) || !static_cast<bool>(get(state,"choiceMenuFocused"))) return;
                    interactionAction_="choice-keyboard"; ++interactionStep_;
                    break;
                case 72:
                    if (!compiled || !uiConsistent || str(state,"flowChoiceValue")!="square") return;
                    check(source==choiceSource_.replace("shape: triangle","shape: square"),"Signal Flow dropdown did not update precisely the authored shape.");
                    interactionAction_="undo"; ++interactionStep_;
                    break;
                case 73:
                    if (!compiled || !uiConsistent || source!=choiceSource_) return;
                    if (auto choices=get(compile,"choices");auto* list=choices.getArray())
                        for(const auto& choice:*list) if(str(choice,"name")=="bipolar") knobId_=str(choice,"id");
                    if (!choiceReady(knobId_)) return;
                    interactionAction_="choice-flow"; ++interactionStep_;
                    break;
                case 74:
                    if (!static_cast<bool>(get(state,"choiceMenu"))) return;
                    interactionAction_="choice-pick:true"; ++interactionStep_;
                    break;
                case 75:
                    if (!compiled || !uiConsistent || str(state,"flowChoiceValue")!="true") return;
                    check(source==choiceSource_.replace("bipolar: false","bipolar: true"),"Boolean dropdown changed unrelated source.");
                    interactionAction_="undo"; ++interactionStep_;
                    break;
                case 76:
                    if (!compiled || !uiConsistent || source!=choiceSource_ || !choiceReady(knobId_)) return;
                    interactionAction_="choice-flow"; ++interactionStep_;
                    break;
                case 77:
                    if (!static_cast<bool>(get(state,"choiceMenu"))) return;
                    interactionAction_="open:examples/sine-reference.flows"; ++interactionStep_;
                    break;
                case 78:
                    if (!compiled || !uiConsistent || str(document,"path")!="examples/sine-reference.flows") return;
                    check(!static_cast<bool>(get(state,"choiceMenu")),"Switching instruments left a stale choice menu open.");
                    interactionAction_="open:examples/stereo-motion.flows"; ++interactionStep_;
                    break;
                case 79:
                    if (!compiled || !uiConsistent || str(document,"path")!="examples/stereo-motion.flows") return;
                    check(source==choiceSource_,"Dropdown navigation changed an inactive instrument.");
                    if (auto choices=get(compile,"choices");auto* list=choices.getArray())
                        for(const auto& choice:*list) if(str(choice,"name")=="shape") knobId_=str(choice,"id");
                    if (!choiceReady(knobId_)) return;
                    interactionAction_="choice-flow"; ++interactionStep_;
                    break;
                case 80:
                    if (!static_cast<bool>(get(state,"choiceMenu"))) return;
                    check(static_cast<bool>(get(state,"choiceMenuFits")),"Signal Flow dropdown overflowed the viewport.");
                    interactionAction_="choice-hide-flow"; ++interactionStep_;
                    break;
                case 81:
                    if (static_cast<bool>((*get(state,"panelVisibility").getArray())[2])) return;
                    check(!static_cast<bool>(get(state,"choiceMenu")) && str(state,"focusedEditorGroup").isNotEmpty(),"Hiding Signal Flow left its portal menu or keyboard focus behind.");
                    interactionAction_="choice-show-flow"; ++interactionStep_;
                    break;
                case 82:
                    if (!static_cast<bool>((*get(state,"panelVisibility").getArray())[2]) || !choiceReady(knobId_)) return;
                    interactionAction_="choice-flow"; ++interactionStep_;
                    break;
                case 83:
                    if (!static_cast<bool>(get(state,"choiceMenu")) || !static_cast<bool>(get(state,"choiceMenuFocused"))) return;
                    interactionAction_="choice-focus-code"; ++interactionStep_;
                    break;
                case 84:
                    if (static_cast<bool>(get(state,"choiceMenu"))) return;
                    check(source==choiceSource_,"Leaving a menu changed source.");
                    if (!choiceReady(knobId_)) return;
                    interactionAction_="choice-flow"; ++interactionStep_;
                    break;
                case 85:
                    if (!static_cast<bool>(get(state,"choiceMenu"))) return;
                    std::cout << "Choice UI: code click/Escape/select; graph keyboard typeahead/boolean select; exact source edits and one-step undo; menus close on instrument switch, panel hide and focus exit.\n";
                    interactionAction_="assistant-markdown"; ++interactionStep_;
                    break;
                case 86: {
                    const auto markdown=get(state,"assistantMarkdown");
                    if (!static_cast<bool>(get(markdown,"ready"))) return;
                    check(static_cast<bool>(get(markdown,"bold")) && static_cast<bool>(get(markdown,"table")) && static_cast<bool>(get(markdown,"copy")),"Saved assistant reply did not render Markdown and code controls.");
                    check(str(markdown,"code")=="raw = osc(pitch: key.pitch, level: -6db)" && !static_cast<bool>(get(markdown,"unsafe")),"Assistant code changed or raw HTML executed.");
                    std::cout << "Assistant UI: persisted reply loads without a model connection; heading/bold/table/Flows code render; HTML stays inert.\n";
                    interactionAction_="assistant-activity-audit"; interactionStep_=861;
                    break;
                }
                case 861: {
                    const auto audit=get(state,"assistantActivity");if(!static_cast<bool>(get(audit,"done")))return;
                    check(str(audit,"error").isEmpty(),str(audit,"error").toRawUTF8());
                    std::cout << "Assistant activity UI: actual backend events keep completed steps, collapse/expand, tool failure and the original Markdown answer.\n";
                    interactionAction_="assistant-history";interactionStep_=87;break;
                }
                case 87:
                    if (str(get(state,"assistantHistory"),"title")!="Markdown test") return;
                    interactionAction_="assistant-row:rename"; ++interactionStep_; break;
                case 88:
                    if (!static_cast<bool>(get(get(state,"assistantHistory"),"rename"))) return;
                    interactionAction_="assistant-rename-save"; ++interactionStep_; break;
                case 89:
                    if (str(get(state,"assistantHistory"),"renameValue")!="UI history renamed") return;
                    interactionAction_="assistant-rename-submit"; ++interactionStep_; break;
                case 90:
                    if (str(get(state,"assistantHistory"),"title")!="UI history renamed") return;
                    interactionAction_="assistant-row:archive"; ++interactionStep_; break;
                case 91:
                    if (str(get(state,"assistantHistory"),"title").isNotEmpty()) return;
                    interactionAction_="assistant-folder:archived"; ++interactionStep_; break;
                case 92:
                    if (str(get(state,"assistantHistory"),"title")!="UI history renamed") return;
                    interactionAction_="assistant-open"; ++interactionStep_; break;
                case 93:
                    if (!static_cast<bool>(get(get(state,"assistantHistory"),"readonly"))) return;
                    check(static_cast<bool>(get(get(state,"assistantMarkdown"),"ready")),"Archived conversation lost its readable response.");
                    interactionAction_="assistant-history"; ++interactionStep_; break;
                case 94:
                    if (str(get(state,"assistantHistory"),"title")!="UI history renamed") return;
                    interactionAction_="assistant-row:restore"; ++interactionStep_; break;
                case 95:
                    if (str(get(state,"assistantHistory"),"title").isNotEmpty()) return;
                    interactionAction_="assistant-folder:active"; ++interactionStep_; break;
                case 96:
                    if (str(get(state,"assistantHistory"),"title")!="UI history renamed") return;
                    interactionAction_="assistant-row:trash"; ++interactionStep_; break;
                case 97:
                    if (str(get(state,"assistantHistory"),"title").isNotEmpty()) return;
                    interactionAction_="assistant-folder:trash"; ++interactionStep_; break;
                case 98:
                    if (str(get(state,"assistantHistory"),"title")!="UI history renamed") return;
                    interactionAction_="assistant-row:restore"; ++interactionStep_; break;
                case 99:
                    if (str(get(state,"assistantHistory"),"title").isNotEmpty()) return;
                    interactionAction_="assistant-folder:active"; ++interactionStep_; break;
                case 100:
                    if (str(get(state,"assistantHistory"),"title")!="UI history renamed") return;
                    interactionAction_="assistant-open"; ++interactionStep_; break;
                case 101:
                    if (static_cast<bool>(get(get(state,"assistantHistory"),"open")) || !static_cast<bool>(get(get(state,"assistantMarkdown"),"ready"))) return;
                    check(!static_cast<bool>(get(get(state,"assistantHistory"),"readonly")),"Restored conversation remained read-only.");
                    std::cout << "Assistant history UI: inline rename, archive/read/restore, Trash/restore and conversation selection passed.\n";
                    interactionAction_="sequencer-open"; ++interactionStep_; break;
                case 102:
                    if (str(state,"sequencerSource").isEmpty()) return;
                    check(get(state,"sequencerSourceOptions").getArray()->contains("sample:Samples/flows-kick.wav") && get(state,"sequencerSourceOptions").getArray()->contains("instrument:examples/sine-reference.flows"),"Sequencer source chooser is missing samples or instruments.");
                    interactionAction_="sequencer-source:sample:Samples/flows-kick.wav"; ++interactionStep_; break;
                case 103: {
                    const auto sequence=get(snapshot,"sequencer");const auto units=get(get(sequence,"sound"),"lanes");const auto sound=units.isArray()?(*units.getArray())[0]:get(sequence,"sound");
                    if (str(sequence,"soundStatus")!="ready" || str(sound,"kind")!="sample" || str(state,"sequencerSource")!="sample:Samples/flows-kick.wav") return;
                    check(str(document,"path")=="examples/stereo-motion.flows","Choosing a sequencer sample activated another editor.");
                    check(static_cast<int>(get((*get(sequence,"rows").getArray())[0],"note"))==48 && str(state,"samplePlayback")=="one_shot" && static_cast<bool>(get(state,"sampleGateDisabled")),"New sample must use C3, one-shot playback and an inactive gate slider.");
                    interactionAction_="sequencer-gate"; interactionStep_=1031; break;
                }
                case 1031: {
                    const auto sequence=get(snapshot,"sequencer"), sound=(*get(get(sequence,"sound"),"lanes").getArray())[0];
                    if(str(sound,"playback")!="gated" || str(state,"samplePlayback")!="gated" || static_cast<bool>(get(state,"sampleGateDisabled")) || str(sequence,"soundStatus")!="ready")return;
                    interactionAction_="sequencer-source:instrument:examples/sine-reference.flows"; interactionStep_=104; break;
                }
                case 104: {
                    const auto sequence=get(snapshot,"sequencer");const auto units=get(get(sequence,"sound"),"lanes");const auto sound=units.isArray()?(*units.getArray())[0]:get(sequence,"sound");
                    if (str(sequence,"soundStatus")!="ready" || str(sound,"kind")!="instrument" || str(state,"sequencerSource")!="instrument:examples/sine-reference.flows") return;
                    check(str(document,"path")=="examples/stereo-motion.flows","Choosing a sequencer instrument activated another editor.");
                    interactionAction_="sequencer-source:active"; ++interactionStep_; break;
                }
                case 105:
                    if (str(state,"sequencerSource")!="active") return;
                    interactionAction_="sequencer-mix"; interactionStep_=1051; break;
                case 1051: {
                    const auto sequence=get(snapshot,"sequencer"), metro=get(sequence,"metronome");
                    if(static_cast<double>(get((*get(sequence,"rows").getArray())[0],"volume"))!=-9 || static_cast<int>(get(metro,"division"))!=8 || static_cast<bool>(get(metro,"mute")))return;
                    check(str(document,"path")=="examples/stereo-motion.flows","Mixer controls changed the active editor.");
                    interactionAction_="sequencer-close"; interactionStep_=106; break;
                }
                case 106:
                    if (str(state,"sequencerSource").isNotEmpty()) return;
                    std::cout << "Sequencer UI: grouped sample/instrument choices prepare independently of the active editor; Follow active restores the original mode.\n";
                    interactionAction_="workspace-drop";++interactionStep_;break;
                case 107: {
                    bool imported=false;for(const auto& file:*get(snapshot,"files").getArray())if(str(file,"path")=="AI-gen/drop-ui.flows")imported=true;
                    if(!imported)return;
                    check(workspace_.getChildFile("AI-gen/drop-ui.flows").existsAsFile(),"Explorer file drop was not saved to its folder");
                    std::cout << "Workspace UI: external File/DataTransfer drop saved inside the selected AI-gen folder.\n";
                    processor_->request("ai.models.save",object({{"name","Local first"},{"model","first"},{"endpoint","http://localhost:1234/v1"}}));
                    testModelId_=str(get(processor_->snapshot(),"ai"),"selectedId");
                    processor_->request("ai.models.save",object({{"name","Local second"},{"model","second"},{"endpoint","http://localhost:1234/v1"}}));
                    interactionAction_.clear();++interactionStep_;break;
                }
                case 108: {
                    const auto models=get(state,"assistantModels"), names=get(models,"names");
                    if(names.size()!=2)return;
                    check(names[0].toString()=="Local first" && names[1].toString()=="Local second","Model picker lost saved profiles.");
                    check(str(models,"reference").isEmpty(),"Assistant implicitly attached the active instrument.");
                    interactionAction_="assistant-model-first";++interactionStep_;break;
                }
                case 109:
                    if(str(get(snapshot,"ai"),"selectedId")!=testModelId_ || str(get(state,"assistantModels"),"selected")!=testModelId_)return;
                    interactionAction_="assistant-model-settings";++interactionStep_;break;
                case 110:
                    if(!static_cast<bool>(get(get(state,"assistantModels"),"global")))return;
                    check(static_cast<int>(get(get(state,"assistantModels"),"savedCount"))==3,"Global model editor lost saved profiles or New model option.");
                    interactionAction_="assistant-model-close";++interactionStep_;break;
                case 111: {
                    if(static_cast<bool>(get(state,"aiConfigurationOpen")))return;
                    std::cout << "Assistant models UI: global profiles, footer selection, settings and optional workspace reference passed.\n";
                    processor_->request("sequencer.play",object({{"playing",false}}));
                    processor_->request("keyboard.panic",object({}));
                    processor_->request("transport.set",object({{"bpm",120}}));
                    // A sustained envelope and explicit audio output make the
                    // final live-display checks independent of prior edits.
                    const String performanceSource=R"FLOWS(import standard as std
from lib.osc import ClassicSine
from lib.env import ClassicADSR

Inst.sigstream(std.event.keys.poly()) as key:
    osc = ClassicSine.make()
    env = ClassicADSR.make()
    raw = osc(pitch: key.pitch, level: -6db)
    amp = env(gate: key.gate, trigger: key.trigger, A: 20ms, D: 80ms, S: 0.65, R: 600ms)
    output = raw * amp * key.velocity
    p_output = probe(output)
    return output
)FLOWS";
                    capturePath_="live-performance-regression.flows";
                    check(workspace_.getChildFile(capturePath_).replaceWithData(performanceSource.toRawUTF8(),performanceSource.getNumBytesAsUTF8()),"Could not create isolated live performance fixture.");
                    noteSent_=true; // The final phases send their own note on/off.
                    processor_->request("document.open",object({{"path",capturePath_}}));
                    observationReadyAt_={}; interactionStep_=1111;break;
                }
                case 1111:
                    if(!compiled || !uiConsistent || str(document,"path")!=capturePath_ || !static_cast<bool>(get(state,"graphReady")))return;
                    interactionAction_="performance-observation-ready";interactionStep_=112;break;
                case 112:
                    if(!compiled || !uiConsistent || str(document,"path")!=capturePath_ || get(capture,"programRevision")!=get(document,"revision"))return;
                    if(static_cast<int>(get(get(snapshot,"observation"),"voice"))!=-1 || str(get(snapshot,"observation"),"projection")!="mid")return;
                    if(!get(get(snapshot,"observation"),"signals").getArray() || !get(get(snapshot,"observation"),"signals").getArray()->contains("raw"))return;
                    if(!get(state,"probeCapsules").getArray() || !get(state,"probeCapsules").getArray()->contains("raw"))return;
                    if(str(capture,"captureMode")!="performance" || !static_cast<bool>(get(capture,"captureIdle")))return;
                    if(observationReadyAt_==Clock::time_point{}){observationReadyAt_=Clock::now();heldCaptureEnd_=captureEnd;}
                    check(captureEnd==0 && get(capture,"signals").size()==0,"Stopped transport retained a native signal.");
                    if(static_cast<int>(get(get(state,"stoppedPreview"),"idle"))==0 || static_cast<int>(get(get(state,"stoppedPreview"),"traces"))!=0)return;
                    if(Clock::now()-observationReadyAt_<std::chrono::milliseconds(200))return;
                    keyCaptureEpoch_=static_cast<juce::int64>(get(capture,"keyEpoch"));
                    processor_->request("keyboard.noteOn",object({{"noteId",9701},{"pitch",60},{"velocity",.8}}));
                    ++interactionStep_;break;
                case 113:
                    if(static_cast<juce::int64>(get(capture,"keyEpoch"))==keyCaptureEpoch_ || static_cast<bool>(get(capture,"captureIdle")))return;
                    check(!static_cast<bool>(get(capture,"captureHeld")),"A new manual key automatically held its display.");
                    check(captureEnd>heldCaptureEnd_,"New key did not begin a fresh capture.");
                    keyCaptureEpoch_=static_cast<juce::int64>(get(capture,"keyEpoch"));
                    heldCaptureEnd_=captureEnd;heldSequence_=static_cast<juce::int64>(get(capture,"sequence"));
                    ++interactionStep_;break;
                case 114: {
                    check(!static_cast<bool>(get(capture,"captureHeld")) && !static_cast<bool>(get(capture,"captureIdle")),"A held manual key froze or became idle after one beat.");
                    // At 120 BPM and 48 kHz, two beats require 48000 samples.
                    // Assert sample progress, not a wall-clock delay alone.
                    if(captureEnd-heldCaptureEnd_<48000)return;
                    const auto previews=get(state,"stoppedPreview");
                    if(static_cast<int>(get(previews,"liveCapsules"))==0 || static_cast<int>(get(previews,"liveFlow"))==0 || static_cast<int>(get(previews,"traces"))<20)return;
                    check(static_cast<juce::int64>(get(capture,"sequence"))>heldSequence_ && static_cast<int>(get(capture,"voices"))>0,"Held-note observation stopped advancing while the envelope sustained.");
                    check(!static_cast<bool>(get(previews,"lastKey")) && !static_cast<bool>(get(previews,"dimmedLive")) && static_cast<int>(get(previews,"heldCapsules"))==0 && static_cast<int>(get(previews,"heldFlow"))==0,"Sustained live previews were automatically marked held or dimmed.");
                    for(const auto& signal:*get(capture,"signals").getArray())
                        check(!static_cast<bool>(get(signal,"beatAvailable")),"Manual note generated a transport beat sweep.");
                    pitchCapsuleHash_=str(get(previews,"pitchCapsule"),"hash");pitchFlowHash_=str(get(previews,"pitchFlow"),"hash");
                    check(pitchCapsuleHash_.isNotEmpty()&&pitchFlowHash_.isNotEmpty(),"Missing actual key.pitch preview canvases.");
                    heldCaptureEnd_=captureEnd;heldPixelsRecorded_=false;
                    processor_->request("keyboard.noteOff",object({{"noteId",9701},{"pitch",60}}));
                    ++interactionStep_;break;
                }
                case 115: {
                    const auto previews=get(state,"stoppedPreview");
                    check(!static_cast<bool>(get(capture,"captureHeld")),"Release automatically held the signal display.");
                    if(!static_cast<bool>(get(capture,"captureIdle"))) {
                        for(const auto& signal:*get(capture,"signals").getArray())
                            check(!static_cast<bool>(get(signal,"beatAvailable")),"Release or quiet drain generated a transport beat sweep.");
                        if(captureEnd>heldCaptureEnd_+4096 && static_cast<int>(get(capture,"voices"))>0 && static_cast<int>(get(previews,"liveCapsules"))>0 && static_cast<int>(get(previews,"liveFlow"))>0) {
                            check(str(get(previews,"pitchCapsule"),"hash")==pitchCapsuleHash_&&str(get(previews,"pitchFlow"),"hash")==pitchFlowHash_,"Constant key.pitch kept scanning after note-off.");
                            heldPixelsRecorded_=true;
                        }
                        return;
                    }
                    check(heldPixelsRecorded_,"The display did not continue capturing the sounding ADSR release after note-off.");
                    check(captureEnd==0 && get(capture,"signals").size()==0,"Completed release retained native signal data.");
                    if(static_cast<int>(get(previews,"idle"))==0 || static_cast<int>(get(previews,"traces"))!=0)return;
                    heldCaptureEnd_=captureEnd;
                    processor_->request("keyboard.noteOn",object({{"noteId",9702},{"pitch",72},{"velocity",.8}}));
                    ++interactionStep_;break;
                }
                case 116: {
                    if(static_cast<juce::int64>(get(capture,"keyEpoch"))==keyCaptureEpoch_ || static_cast<bool>(get(capture,"captureIdle")))return;
                    check(!static_cast<bool>(get(capture,"captureHeld")),"The next manual key did not resume live observation.");
                    check(captureEnd>heldCaptureEnd_,"Retrigger did not replace the previous acquisition.");
                    const auto previews=get(state,"stoppedPreview");
                    if(static_cast<int>(get(previews,"liveCapsules"))==0 || static_cast<int>(get(previews,"liveFlow"))==0 || static_cast<int>(get(previews,"traces"))<20)return;
                    check(!static_cast<bool>(get(previews,"lastKey")) && !static_cast<bool>(get(previews,"dimmedLive")),"The resumed note retained the obsolete held presentation.");
                    std::cout << "Performance capture: sustained notes stay LIVE; release keeps exact key.pitch pixels without a beat sweep while ADSR continues, then settles to idle and the next key resumes.\n";
                    processor_->request("keyboard.panic",object({}));
                    interactionAction_="editor-completion-audit";++interactionStep_;break;
                }
                case 117: {
                    const auto audit=get(state,"editorAudit");
                    if(!static_cast<bool>(get(audit,"done")))return;
                    if(str(audit,"error").isNotEmpty())throw std::runtime_error("Editor audit: "+str(audit,"error").toStdString());
                    if(!compiled)return;
                    std::cout << "Editor: actual WebKit key-member completion and Tab acceptance on unfinished source; cursor/text/click coordinates after font/size changes from 10 to 32px, with aligned line-number glyphs; exact insertion before inline widgets.\n";
                    if (editorLayoutOnly_) {
                        if (!editorLayoutShort_) {
                            editorLayoutShort_=true;
                            processor_->request("document.open",object({{"path","examples/sine-reference.flows"}}));
                            interactionStep_=119;
                        } else takeSnapshot();
                        break;
                    }
                    const auto demo=File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("examples/ADCxGather/07-developer/01-formula-oscillator.flows").loadFileAsString();
                    check(demo.contains("from lib.dsp import Phasor"),"Missing developer demonstration source.");
                    processor_->request("document.apply",object({{"documentId",get(document,"id")},{"baseRevision",get(document,"revision")},{"text",demo}}));
                    ++interactionStep_;break;
                }
                case 118:
                    if(!compiled || !uiConsistent)return;
                    check(source.contains("from lib.dsp import Phasor"),"Developer demo source was not loaded.");
                    check(get(state,"probeCapsules").getArray()->contains("osc.phase") && get(state,"probeCapsules").getArray()->contains("raw"),"Compiled developer phase/output is missing from the real editor.");
                    std::cout << "Developer demo UI: complete HarmonicOsc/Phasor/ADSR source compiles in the native host and renders phase/output previews.\n";
                    takeSnapshot();break;
                case 119:
                    if(!compiled || !uiConsistent || str(document,"path")!="examples/sine-reference.flows")return;
                    interactionAction_="editor-completion-audit";interactionStep_=117;break;
                default: throw std::runtime_error("Unknown UI integration phase.");
            }
        } catch (const std::exception& exception) { finish(false, String(exception.what()) + "\nLast page: " + lastPage_); }
    }

    void snapshotResult(NSImage* image, NSError* error) {
        if (finished_) return;
        try {
            if (error != nil) throw std::runtime_error("WebView raster snapshot failed: " + std::string([error.localizedDescription UTF8String]));
            check(image != nil, "WebView snapshot returned no image.");
            auto* cgImage = [image CGImageForProposedRect:nullptr context:nil hints:nil];
            check(cgImage != nullptr, "WebView snapshot has no raster representation.");
            const auto width = CGImageGetWidth(cgImage), height = CGImageGetHeight(cgImage);
            check(width >= 100 && height >= 100 && width <= 4096 && height <= 4096,
                  "WebView snapshot has empty or unexpected dimensions.");
            std::vector<std::uint8_t> pixels(width * height * 4);
            auto* colourSpace = CGColorSpaceCreateDeviceRGB();
            auto* context = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4,
                colourSpace, static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(colourSpace);
            check(context != nullptr, "Could not decode WebView snapshot pixels.");
            CGContextDrawImage(context, CGRectMake(0, 0, width, height), cgImage);
            CGContextRelease(context);
            std::array<std::size_t, 4096> colours{};
            std::size_t visible = 0;
            int darkest = 255, brightest = 0;
            for (std::size_t pixel = 0; pixel < width * height; ++pixel) {
                const auto* rgba = pixels.data() + pixel * 4;
                if (rgba[3] < 16) continue;
                ++visible;
                const auto bucket = (static_cast<unsigned>(rgba[0] >> 4) << 8) |
                    (static_cast<unsigned>(rgba[1] >> 4) << 4) | (rgba[2] >> 4);
                ++colours[bucket];
                const auto brightness = (54 * rgba[0] + 183 * rgba[1] + 19 * rgba[2]) / 256;
                darkest = std::min(darkest, brightness);
                brightest = std::max(brightest, brightness);
            }
            const auto colourCount = std::count_if(colours.begin(), colours.end(), [](auto count) { return count != 0; });
            const auto dominant = *std::max_element(colours.begin(), colours.end());
            const auto visibleFraction = static_cast<double>(visible) / static_cast<double>(width * height);
            std::cout << "WebView raster: " << width << 'x' << height << ", visible=" << visibleFraction
                      << ", colour buckets=" << colourCount << ", brightness range=" << brightest - darkest << '\n';
            // Preserve even a bad image when explicitly requested, for diagnosing
            // an all-transparent or solid-colour regression without desktop capture.
            if (snapshotFile_ != File()) {
                auto* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:cgImage];
#if !__has_feature(objc_arc)
                [bitmap autorelease];
#endif
                auto* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                check(png != nil && snapshotFile_.getParentDirectory().createDirectory().wasOk() &&
                      snapshotFile_.replaceWithData(png.bytes, png.length), "Could not save requested WebView PNG.");
                std::cout << "WebView PNG: " << snapshotFile_.getFullPathName() << '\n';
            }
            check(visibleFraction > .5, "WebView raster is predominantly transparent/empty.");
            check(colourCount >= 8 && brightest - darkest >= 32 &&
                  static_cast<double>(dominant) < static_cast<double>(visible) * .995,
                  "WebView raster is solid-colour or lacks rendered UI contrast.");
            check(window_->identity() == baseline_ && window_->webView() == webIdentity_,
                  "Native window/WebView identity changed during snapshot.");
            if (editorLayoutOnly_) { finish(true, "editor completion, cursor/click coordinates and gutter alignment for long and short documents at 10, 16, 23 and 32px; nonempty WebView raster"); return; }
            finish(true, "stable transparent peer/WebView; native bridge; font shortcuts; read-only graph; knob source edit/undo; persistent panel resize; continuous capture/Inspector; shared observation Hold/Resume; computer-key repeat handling; nonempty WebView raster");
        } catch (const std::exception& exception) { finish(false, exception.what()); }
    }

    void finish(bool passed, const String& message) {
        if (finished_) return;
        finished_ = true;
        passed_ = passed;
        stopTimer();
        setApplicationReturnValue(passed ? 0 : 1);
        auto& output = passed ? std::cout : std::cerr;
        output << "Window integration " << (passed ? "passed: " : "FAILED: ") << message << '\n';
        if (workspace_ != File() && (!passed || keepWorkspace_)) output << "Workspace: " << workspace_.getFullPathName() << '\n';
        output.flush();
        quit();
    }
};
}

START_JUCE_APPLICATION(WindowTests)
