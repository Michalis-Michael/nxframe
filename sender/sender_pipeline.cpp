/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: sender/sender_pipeline.cpp
 * Description: Implements the sender pipeline lifecycle and worker coordination.
 */

#include "sender/sender_pipeline.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <fstream>
extern "C" {
#include <libavutil/pixdesc.h>
}
#include <thread>

#include "stage_timing.h"

SenderPipeline::SenderPipeline(size_t captureQueueFrames)
    : captureQueueFrames_(captureQueueFrames),
      videoQ_(captureQueueFrames),
      audioQ_(128),
      videoPktQ_(32),
      audioPktQ_(64)
{
}

SenderPipeline::~SenderPipeline()
{
    shutdown();
}

bool SenderPipeline::initialize(const Config& config)
{
    if (captureQueueFrames_ != 1 && captureQueueFrames_ != 2 && captureQueueFrames_ != 4) {
        std::cerr << "[Main] Capture queue must contain 1, 2 or 4 frames.\n";
        return false;
    }
    config_ = config;
    nxframe::DashboardState display;
    display.input = config_.inputType + " " + config_.cardInput;
    display.device = display.input;
    if(config_.inputType=="decklink") display.genlock="WAITING";
    if(config_.inputType=="test") { display.source="Test generator"; display.signal="SYNTHETIC"; }
    display.transport = config_.transport == OutputManager::SenderTransport::SRT ? "SRT" : "UDP/RTP";
    if(display.transport!="SRT") display.network="Connectionless (no handshake)";
    display.endpoint = config_.transportAddress + ":" + std::to_string(config_.transportPort);
    // Only descriptive fields are copied. Never copy passwords/stream IDs into telemetry.
    try {
        std::ifstream file(config_.presetFile); json preset; file >> preset;
        const auto& v = preset.contains("video") ? preset.at("video") : preset;
        const auto& opts = v.contains("additional_options") ? v.at("additional_options") : json::object();
        std::ostringstream e;
        e << v.value("codec", std::string("--")) << " / " << v.value("preset", std::string("medium"))
          << " | " << v.value("profile", std::string("--"))
          << " | Target " << v.value("bitrate", 0)/1000000.0 << " Mbps"
          << " | B=" << v.value("max_b_frames", 0);
        if(v.contains("gop")) e << " | GOP " << v.at("gop").value("size",0);
        if(opts.contains("rc-lookahead")) e << " | Lookahead " << opts.at("rc-lookahead");
        if(opts.contains("frame-threads")) e << " | Frame threads " << opts.at("frame-threads");
        else e << " | Frame threads auto";
        if(opts.contains("level-idc")) e << " | Level " << opts.at("level-idc") << " (requested)";
        e << " | Pool " << opts.value("pools",std::string("auto"));
        if(v.contains("tune")) e << " | Tune " << v.at("tune");
        display.codec=v.value("codec",std::string("--"));
        display.preset=v.value("preset",std::string("medium"));
        display.profile=v.value("profile",std::string("--"));
        display.target=nxframe::dashboardNumber(v.value("bitrate",0)/1000000.0)+" Mbps";
        display.bframes=std::to_string(v.value("max_b_frames",0));
        if(v.contains("gop")) display.gop=std::to_string(v.at("gop").value("size",0))+" / "+(v.at("gop").value("closed",false)?"closed":"open");
        auto option=[&](const char* key) { return opts.contains(key) ? (opts.at(key).is_string()?opts.at(key).get<std::string>():opts.at(key).dump()) : std::string("auto"); };
        display.level=option("level-idc")+" (requested)";
        display.threads=option("frame-threads")+" frame / "+option("pools")+" pool";
        display.lookahead=option("rc-lookahead");
        display.output=std::to_string(v.value("width",0))+"x"+std::to_string(v.value("height",0))+" @ "+std::to_string(v.value("framerate",0));
        display.encoder = e.str();
        if(preset.contains("audio")) {
            const auto& a=preset.at("audio");
            display.audio=a.value("codec",std::string("--"))+" | "+std::to_string(a.value("sample_rate",48000))+" Hz";
        }
        if(preset.contains("srt") && display.transport=="SRT") {
            display.transport += " " + preset.at("srt").value("mode",std::string("caller"));
            display.latency=std::to_string(preset.at("srt").value("latency",120))+" ms";
        }
    } catch (...) { display.encoder="Preset details unavailable"; }
    previousTimingEnabled_=stage_timing::enabled();
    stage_timing::set_enabled(true,stage_timing::verbose_enabled());
    dashboardStarted_=std::chrono::steady_clock::now();
    nxframe::senderDashboard().start(display,stage_timing::verbose_enabled(),true);

    if (!inputManager_.init(config_.inputType, config_.cardInput, config_.allowTestFallback)) {
        std::cerr << "[Main] Failed to initialize input source.\n";
        return false;
    }

    encoder_ = EncoderManager::createEncoder(config_.presetFile, config_.useSwsForPixelConversion);
    bool encoderReady = false;
    try {
        encoderReady = encoder_ && encoder_->initialize();
    } catch (const std::exception& e) {
        std::cerr << "[Main] Encoder initialization exception: " << e.what() << "\n";
        encoderReady = false;
    }
    if (!encoderReady) {
        std::cerr << "[Main] Encoder initialization failed.\n";
        inputManager_.stopCapture();
        return false;
    }

    nxframe::senderDashboard().update([&](nxframe::DashboardState& d) {
        auto* c=encoder_->getVideoCodecContext();
        if(c) {
            const char* pf=av_get_pix_fmt_name(c->pix_fmt);
            const char* prim=av_color_primaries_name(c->color_primaries);
            const char* transfer=av_color_transfer_name(c->color_trc);
            d.encoder += " | " + std::to_string(c->width)+"x"+std::to_string(c->height)+" / "+(pf?pf:"--");
            const auto* pixel=av_pix_fmt_desc_get(c->pix_fmt);
            std::string pixelLabel=pf?pf:"--";
            if(pixel && pixel->nb_components==3 && !(pixel->flags & AV_PIX_FMT_FLAG_RGB)) {
                pixelLabel=std::string(pixel->log2_chroma_w==1?(pixel->log2_chroma_h==1?"4:2:0":"4:2:2"):"4:4:4")+" "+std::to_string(pixel->comp[0].depth)+"-bit";
            }
            d.output=std::to_string(c->width)+"x"+std::to_string(c->height)+
                (c->framerate.den>0 ? " @"+nxframe::dashboardNumber(double(c->framerate.num)/c->framerate.den) : "")+" / "+pixelLabel;
            d.colour=std::string(prim?prim:"--")+" / "+(transfer?transfer:"--");
            d.encoder += " | Colour " + std::string(prim?prim:"--")+" / "+(transfer?transfer:"--");
        }
        size_t channels=0;
        const auto audioContexts=encoder_->getAudioCodecContexts();
        for(auto* a:audioContexts) if(a) channels+=a->ch_layout.nb_channels;
        if(!audioContexts.empty() && audioContexts.front()) {
            auto* a=audioContexts.front();
            d.audio=std::string(avcodec_get_name(a->codec_id))+" / "+std::to_string(a->sample_rate)+" Hz";
        }
        d.audio += " | Output " + std::to_string(channels)+" ch";
    });
    OutputManager::SenderInitOptions outputOptions;
    outputOptions.tsDebug = config_.tsDebug;
    outputOptions.tsCapturePath = config_.tsCapturePath;
    outputOptions.externalStopFlag = config_.externalStopFlag;

    if (!outputManager_.initializeSender(config_.presetFile,
                                         config_.transport,
                                         config_.transportAddress,
                                         config_.transportPort,
                                         *encoder_,
                                         outputOptions)) {
        inputManager_.stopCapture();
        return false;
    }

    VideoEncodeWorker::Config videoConfig;
    videoConfig.forceCopy = config_.forceCopy;

    videoWorker_.reset(new VideoEncodeWorker(*encoder_,
                                             videoQ_,
                                             audioQ_,
                                             videoPktQ_,
                                             audioPktQ_,
                                             telemetry_,
                                             stop_,
                                             transportRecovering_,
                                             encodedVideoDiscontinuity_,
                                             videoConfig));
    audioWorker_.reset(new AudioEncodeWorker(*encoder_,
                                             videoQ_,
                                             audioQ_,
                                             videoPktQ_,
                                             audioPktQ_,
                                             telemetry_,
                                             stop_,
                                             transportRecovering_));

    initialized_ = true;
    return true;
}

int SenderPipeline::run()
{
    if (!initialized_ || !encoder_) {
        return -1;
    }

    // The transport may not be connected yet, especially in SRT listener mode.
    // Start capture/encoding immediately, but gate encoded packet output until
    // OutputManager has a connected socket and has requested a fresh keyframe.
    transportRecovering_.store(true, std::memory_order_release);
    waitForFreshKeyframe_.store(true, std::memory_order_release);

    if (!inputManager_.startProducer(stop_, videoQ_, audioQ_, &telemetry_)) {
        std::cerr << "[Main] Failed to start producer thread.\n";
        outputManager_.shutdownSender();
        inputManager_.stopCapture();
        return -1;
    }
    producerStarted_ = true;

    videoWorker_->start();
    audioWorker_->start();

    if (!outputManager_.startSenderRuntime(videoPktQ_,
                                           audioPktQ_,
                                           *encoder_,
                                           telemetry_,
                                           stop_,
                                           videoWorker_->doneFlag(),
                                           audioWorker_->doneFlag(),
                                           transportRecovering_,
                                           waitForFreshKeyframe_,
                                           encodedVideoDiscontinuity_)) {
        stop_.request_stop();
    } else {
        outputRuntimeStarted_ = true;
    }

    startPerfThread();

    while (!(config_.externalStopFlag && config_.externalStopFlag->load(std::memory_order_acquire)) &&
           !stop_.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    shutdown();
    return 0;
}

void SenderPipeline::startPerfThread()
{
    auto reference=inputManager_.referenceMonitor();
    perfThread_ = std::thread([this,reference]() {
        uint64_t lastIn=0,lastEncoded=0,lastAudio=0,lastBytes=0,lastEvictions=0,lastBudget=0,lastCalls=0;
        auto previous=dashboardStarted_;
        auto lastPrinted=previous;
        nxframe::SystemResourceMonitor resources;
        resources.sample();
        std::map<std::string,stage_timing::Sample> timings;
        while (!stop_.stop_requested()) {
            // Short waits keep shutdown responsive.
            for(int i=0;i<10 && !stop_.stop_requested();++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto now=std::chrono::steady_clock::now();
            const double seconds=std::chrono::duration<double>(now-previous).count();
            if(seconds<=0) continue;
            nxframe::DashboardInterval p;
            p.resources=resources.sample();
            if(reference) {
                const auto ref=reference->sample();
                nxframe::senderDashboard().update([&](nxframe::DashboardState& d) { d.genlock=ref.first; d.referenceFormat=ref.second; });
            }
            p.seconds=seconds; p.uptime=std::chrono::duration<double>(now-dashboardStarted_).count();
            const auto in=telemetry_.inVideo.load(), enc=telemetry_.encVideo.load();
            const auto audio=telemetry_.encAudio.load(), bytes=telemetry_.sendBytes.load();
            p.capture=(in-lastIn)/seconds; p.encoded=(enc-lastEncoded)/seconds;
            p.audio=(audio-lastAudio)/seconds; p.mbps=(bytes-lastBytes)*8.0/1000000.0/seconds;
            lastIn=in; lastEncoded=enc; lastAudio=audio; lastBytes=bytes; previous=now;
            auto budget=telemetry_.encodeOverBudget.load(), calls=telemetry_.encodeCalls.load();
            p.overBudget=budget-lastBudget; p.encodeCalls=calls-lastCalls; lastBudget=budget; lastCalls=calls;
            p.encodedTotal=enc; p.evictions=videoQ_.evicted_oldest();
            p.evictionDelta=p.evictions-lastEvictions; lastEvictions=p.evictions;
            p.vq=videoQ_.size(); p.aq=audioQ_.size(); p.vpq=videoPktQ_.size(); p.apq=audioPktQ_.size();
            p.peakVq=telemetry_.peakVideoQ.load();
            p.captureQueueFrames=captureQueueFrames_;
            p.queueDelay=telemetry_.videoQueueDelay.takeInterval();
            p.muxErrors=telemetry_.muxFail.load(); p.sendErrors=telemetry_.sendFail.load();
            p.overflowV=telemetry_.dropVideoPktBackpressure.load(); p.overflowA=telemetry_.dropAudioPktBackpressure.load();
            p.recoveryV=telemetry_.dropVideoWhileRecovering.load(); p.recoveryA=telemetry_.dropAudioWhileRecovering.load();
            p.waitingV=telemetry_.dropVideoWhileWaitingKeyframe.load(); p.waitingA=telemetry_.dropAudioWhileWaitingKeyframe.load();
            p.repairs=telemetry_.muxVideoDtsRepairs.load()+telemetry_.muxVideoPtsRepairs.load();
            p.recovering=transportRecovering_.load(); p.keyframeWait=waitForFreshKeyframe_.load();
            auto* ctx=encoder_->getVideoCodecContext();
            if(ctx && ctx->framerate.num>0) p.budgetMs=1000.0*ctx->framerate.den/ctx->framerate.num;
            for(const auto& sample:stage_timing::registry().snapshot()) {
                auto it=timings.find(sample.name);
                const uint64_t oldCalls=it==timings.end()?0:it->second.calls;
                const uint64_t oldTotal=it==timings.end()?0:it->second.total_ns;
                auto& t=p.timing[sample.name];
                t.calls=sample.calls>=oldCalls?sample.calls-oldCalls:sample.calls;
                const uint64_t delta=sample.total_ns>=oldTotal?sample.total_ns-oldTotal:sample.total_ns;
                t.avgMs=t.calls?double(delta)/t.calls/1000000.0:0;
                t.maxMs=double(sample.max_ns)/1000000.0;
                timings[sample.name] = sample;
            }
            dashboardLast_=p;
            nxframe::senderDashboard().recordInterval(p);
            if(nxframe::senderDashboard().verbose()) {
                telemetry_.report(p.vq,p.aq,p.vpq,p.apq);
                std::ostringstream detail; detail<<std::fixed<<std::setprecision(3);
                for(const auto& kv:p.timing) if(kv.second.calls)
                    detail<<kv.first<<":avg="<<kv.second.avgMs<<"ms,max_session="<<kv.second.maxMs<<"ms,n="<<kv.second.calls<<"  ";
                std::cout<<"[TIMING] "<<detail.str()<<"\n";
            }
            // A terminal redraws every second; a pipe receives snapshots every two seconds.
            if(nxframe::senderDashboard().interactive() || now-lastPrinted>=std::chrono::seconds(2) || stop_.stop_requested()) {
                nxframe::senderDashboard().render(p,stop_.stop_requested()); lastPrinted=now;
            }
        }
    });
}

void SenderPipeline::stopQueues()
{
    videoQ_.stop();
    audioQ_.stop();
    videoPktQ_.stop();
    audioPktQ_.stop();
}

void SenderPipeline::joinWorkers()
{
    if (videoWorker_) {
        videoWorker_->join();
    }
    if (audioWorker_) {
        audioWorker_->join();
    }
    if (perfThread_.joinable()) {
        perfThread_.join();
    }
}

void SenderPipeline::shutdown()
{
    stop_.request_stop();
    outputManager_.srtStreamer().requestStop();

    stopQueues();

    if (producerStarted_) {
        inputManager_.stopCapture();
        producerStarted_ = false;
    } else {
        inputManager_.stopCapture();
    }

    joinWorkers();

    if (outputRuntimeStarted_) {
        outputManager_.stopSenderRuntime();
        outputRuntimeStarted_ = false;
    }

    if (initialized_) {
        outputManager_.shutdownSender();
        initialized_ = false;
    }
    if(nxframe::senderDashboard().active()) {
        dashboardLast_.uptime=std::chrono::duration<double>(std::chrono::steady_clock::now()-dashboardStarted_).count();
        dashboardLast_.evictions=videoQ_.evicted_oldest();
        dashboardLast_.encodedTotal=telemetry_.encVideo.load();
        dashboardLast_.muxErrors=telemetry_.muxFail.load(); dashboardLast_.sendErrors=telemetry_.sendFail.load();
        nxframe::senderDashboard().finish(dashboardLast_);
        stage_timing::set_enabled(previousTimingEnabled_,stage_timing::verbose_enabled());
    }
}
