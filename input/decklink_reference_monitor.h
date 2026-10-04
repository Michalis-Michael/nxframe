/* NxFrame - Copyright (c) 2026 Michalis Michael.
 * Governed by the project license / EULA. Read-only reference telemetry.
 */
#pragma once
#include "DeckLinkAPI.h"
#include <memory>
#include <string>
#include <cstdlib>

// Own interface references independently of capture, including during shutdown.
// Called once per reporting interval, never from the frame callback.
class DeckLinkReferenceMonitor {
    template<class T> static std::shared_ptr<T> query(IDeckLink* device,REFIID iid) {
        T* p=nullptr;
        if(!device || device->QueryInterface(iid,reinterpret_cast<void**>(&p))!=S_OK) return {};
        return std::shared_ptr<T>(p,[](T* value) { value->Release(); });
    }
    std::shared_ptr<IDeckLinkStatus> status_;
    std::shared_ptr<IDeckLinkInput> input_;
    bool supportKnown_=false,supported_=false;
public:
    explicit DeckLinkReferenceMonitor(IDeckLink* device):
        status_(query<IDeckLinkStatus>(device,IID_IDeckLinkStatus)),
        input_(query<IDeckLinkInput>(device,IID_IDeckLinkInput)) {
        auto attributes=query<IDeckLinkProfileAttributes>(device,IID_IDeckLinkProfileAttributes);
        if(attributes) supportKnown_=attributes->GetFlag(BMDDeckLinkHasReferenceInput,&supported_)==S_OK;
    }
    std::pair<std::string,std::string> sample() const {
        if(supportKnown_ && !supported_) return {"NOT SUPPORTED","No reference input connector"};
        if(!status_) return {"UNAVAILABLE","Status interface unavailable"};
        bool locked=false;
        if(status_->GetFlag(bmdDeckLinkStatusReferenceSignalLocked,&locked)!=S_OK)
            return {"UNAVAILABLE","Reference lock query unavailable"};
        int64_t mode=bmdModeUnknown;
        bool detected=status_->GetInt(bmdDeckLinkStatusReferenceSignalMode,&mode)==S_OK && mode!=bmdModeUnknown && mode!=0;
        std::string format="Format not reported";
        if(detected) {
            if(mode==bmdModePAL) format="PAL / 625i50";
            else if(mode==bmdModeNTSC) format="NTSC / 525i59.94";
            else if(input_) {
                IDeckLinkDisplayMode* display=nullptr;
                if(input_->GetDisplayMode(static_cast<BMDDisplayMode>(mode),&display)==S_OK && display) {
                    const char* name=nullptr;
                    if(display->GetName(&name)==S_OK && name) { format=name; std::free(const_cast<char*>(name)); }
                    display->Release();
                }
            }
        }
        // False lock alone cannot distinguish a missing cable from invalid sync.
        return {locked?"LOCKED":detected?"UNLOCKED":"NO LOCK / absent or undetected",format};
    }
};
