#include "encoders/encoder_x265.h"
#include "core/caption_a53.h"
#include "stage_timing.h"

#include <fstream>
extern "C" {
#include <libavutil/cpu.h>
}
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <set>
#include <sstream>

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static json preset(bool native = false, bool delayed = false)
{
    return {{"width", 128}, {"height", 96}, {"framerate", 50},
            {"bitrate", 1000000}, {"vbv-maxrate", 1000000}, {"vbv_bufsize", 1000000},
            {"preset", "ultrafast"}, {"tune", delayed ? "" : "zerolatency"},
            {"single_frame_encoding", !delayed},
            {"max_b_frames", delayed ? 2 : 0},
            {"profile", native ? "main422-10" : "main"},
            {"output", {{"bit_depth", native ? 10 : 8}, {"chroma", native ? "422" : "420"}}},
            {"gop", {{"size", 50}, {"min_keyint", 1}, {"closed", true}, {"scenecut", 0}}},
            {"color", {{"primaries", "bt709"}, {"transfer", "bt709"}, {"matrix", "bt709"}}},
            {"additional_options", {{"frame-threads", 1},
                                    {"rc-lookahead", delayed ? 4 : 0}, {"vbv-init", 0.75}}}};
}

static VideoFrame frame(int64_t pts, bool padded = false)
{
    VideoFrame vf;
    vf.width = 128;
    vf.height = 96;
    vf.pix_fmt = AV_PIX_FMT_YUV422P10LE;
    vf.time_base = {1, 50};
    vf.pts = pts;
    vf.linesize[0] = 256 + (padded ? 32 : 0);
    vf.linesize[1] = vf.linesize[2] = 128 + (padded ? 32 : 0);
    vf.buffer_size = (vf.linesize[0] + vf.linesize[1] + vf.linesize[2]) * vf.height;
    vf.buffer = make_shared_u8(vf.buffer_size);
    vf.data[0] = vf.buffer.get();
    vf.data[1] = vf.data[0] + vf.linesize[0] * vf.height;
    vf.data[2] = vf.data[1] + vf.linesize[1] * vf.height;
    for (int p = 0; p < 3; ++p)
        for (int y = 0; y < vf.height; ++y) {
            auto* row = reinterpret_cast<uint16_t*>(vf.data[p] + vf.linesize[p] * y);
            for (int x = 0; x < (p ? vf.width / 2 : vf.width); ++x)
                row[x] = p ? 512 : 64 + ((x + y + pts) % 256);
        }
    return vf;
}

static bool containsIdr(const AVPacket* packet)
{
    for (int i = 0; i + 5 < packet->size; ++i) {
        if (packet->data[i] || packet->data[i + 1]) continue;
        int header = -1;
        if (packet->data[i + 2] == 1) header = i + 3;
        else if (!packet->data[i + 2] && packet->data[i + 3] == 1) header = i + 4;
        if (header >= 0) {
            const int type = (packet->data[header] >> 1) & 63;
            if (type == 19 || type == 20) return true;
        }
    }
    return false;
}

static void invalidPresets()
{
    auto reject = [](const json& value) {
        EncoderX265 enc(value);
        require(!enc.initialize(), "invalid preset was accepted");
        require(!enc.getBlackFrame(), "invalid preset allocated a black frame");
    };
    reject(json::array());
    for (const auto& change : std::vector<json>{
        {{"refs", 0}}, {{"refs", "2"}}, {{"width", -1}}, {{"width", 127}}, {{"height", 95}}, {{"framerate", 29.97}},
        {{"width", 4294967424ULL}}, {{"video", "bad"}}, {{"interlaced", "true"}},
        {{"framerate", 0}}, {{"interlaced", true}}, {{"profile", "main10"}},
        {{"pix_fmt", "invalid"}}, {{"pix_fmt", "rgb24"}}, {{"rate_control", "typo"}},
        {{"gop", {{"size", 0}}}}, {{"gop", {{"size",50}, {"closed",1}}}},
        {{"additional_options", {{"frame-threads",16}}}},
        {{"additional_options", {{"threads",16}}}}, {{"output", {{"bit_depth", 12}, {"chroma", "420"}}}},
        {{"additional_options", {{"aq-strength", "1.0:interlace=tff"}}}},
        {{"additional_options", {{"rc-lookahead", "4oops"}}}},
        {{"additional_options", {{"rd", 99}}}},
        {{"additional_options", {{"threads", 2}, {"frame-threads", 1}}}},
        {{"additional_options", {{"threads", 2}, {"frame_threads", 1}}}},
        {{"additional_options", {{"me", "oops"}}}},
        {{"additional_options", {{"asm", "avx512:rd=0"}}}},
        {{"additional_options", {{"asm", "unknown"}}}},
        {{"additional_options", {{"asm", 0}}}},
        {{"additional_options", {{"rdoq-level",3}}}},
        {{"additional_options", {{"rskip",-1}}}},
        {{"additional_options", {{"early-skip",2}}}},
        {{"additional_options", {{"early-skip",true},{"no_early_skip",true}}}},
        {{"additional_options", {{"rskip-edge-threshold",101}}}},
        {{"additional_options", {{"max-tu-size",12}}}},
        {{"additional_options", {{"nr-intra",2001}}}},
        {{"additional_options", {{"cbqpoffs",-13}}}},
        {{"additional_options", {{"rd",0}}}},
        {{"additional_options", {{"aq-strength",3.1}}}},
        {{"additional_options", {{"psy-rd",5.1}}}},
        {{"additional_options", {{"psy-rdoq",50.1}}}},
        {{"additional_options", {{"limit-tu",1.5}}}},
        {{"additional_options", {{"rdoq-level",1},{"rdoq_level",2}}}},
        {{"additional_options", {{"rskip","2:rd=1"}}}},
        {{"additional_options", {{"unknown-parameter", 1}}}},
        {{"additional_options", {{"rc-lookahead", 0}, {"rc_lookahead", 1}}}}
    }) {
        auto value = preset();
        value.update(change);
        reject(value);
    }
}

static void roundTrip(bool native, bool delayed, bool legacy = false, bool raw = false)
{
    EncoderX265 encoder(preset(native, delayed));
    require(encoder.initialize(), "encoder initialization failed");
    require(encoder.initialize(), "repeated initialization must be idempotent");
    auto* decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC));
    require(decoder != nullptr, "decoder allocation failed");
    decoder->thread_count = 1;
    require(avcodec_open2(decoder, decoder->codec, nullptr) == 0, "decoder open failed");
    AVFrame* decoded = av_frame_alloc();
    std::set<int64_t> seen;
    std::set<int64_t> captions;
    bool forcedIdr = false;
    int packetCount = 0;
    auto receive = [&]() {
        for (;;) {
            const int rc = avcodec_receive_frame(decoder, decoded);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            require(rc >= 0, "decoder receive failed");
            require(decoded->width == 128 && decoded->height == 96, "incorrect decoded dimensions");
            require(decoded->format == (native ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV420P),
                    "incorrect decoded pixel format");
            require(decoded->color_primaries == AVCOL_PRI_BT709, "incorrect static VUI");
            require(seen.insert(decoded->pts).second, "duplicate output timestamp");
            if (auto* sd = av_frame_get_side_data(decoded, AV_FRAME_DATA_A53_CC)) {
                const std::vector<uint8_t> expected{0xfc, 0x94, 0x20};
                require(sd->size == expected.size() && !std::memcmp(sd->data, expected.data(), sd->size),
                        "caption payload changed");
                captions.insert(decoded->pts);
            }
            av_frame_unref(decoded);
        }
    };
    auto consume = [&](std::vector<AVPacketPtr> packets) {
        for (auto& pkt : packets) {
            ++packetCount;
            if (pkt->pts == 7) forcedIdr = containsIdr(pkt.get());
            require(avcodec_send_packet(decoder, pkt.get()) == 0, "decoder send failed");
            receive();
        }
    };
    for (int i = 0; i < 20; ++i) {
        auto vf = frame(i, !legacy && i % 2 == 0);
        if (i == 7) encoder.requestKeyFrame();
        if (i == 3 && !legacy) vf.metadata.caption = nxframe::parseA53CcData(
            reinterpret_cast<const uint8_t*>("\xfc\x94\x20"), 3);
        if (legacy) {
            auto pkt = raw ? encoder.encodeFrame(vf.buffer.get(), vf.pts) :
                             encoder.encodeFrameZeroCopy(vf.buffer, vf.buffer_size, vf.pts);
            std::vector<AVPacketPtr> packets;
            if (pkt) packets.emplace_back(std::move(pkt));
            consume(std::move(packets));
        } else {
            consume(encoder.encodeVideoFramePackets(vf));
        }
        // Inputs are deliberately destroyed immediately with lookahead enabled.
    }
    consume(encoder.flush());
    require(encoder.flush().empty(), "repeated flush returned duplicate packets");
    require(encoder.encodeVideoFramePackets(frame(20)).empty(), "accepted frame after flush");
    require(!encoder.initialize(), "reinitialized a flushed codec");
    require(avcodec_send_packet(decoder, nullptr) == 0, "decoder flush failed");
    receive();
    require(seen.size() == 20 && packetCount == 20, "lost delayed output");
    require(forcedIdr, "requested recovery frame was not an IDR");
    require(legacy || captions == std::set<int64_t>{3}, "captions absent or leaked into another frame");
    av_frame_free(&decoded);
    avcodec_free_context(&decoder);
}

static void invalidFramesAndClock()
{
    EncoderX265 enc(preset(true));
    require(enc.initialize(), "encoder initialization failed");
    auto bad = frame(0);
    bad.width = 64;
    require(enc.encodeVideoFramePackets(bad).empty(), "accepted wrong dimensions");
    bad = frame(0); bad.interlaced = true;
    require(enc.encodeVideoFramePackets(bad).empty(), "accepted interlaced input");
    bad = frame(0); bad.linesize[1] = 1;
    require(enc.encodeVideoFramePackets(bad).empty(), "accepted short stride");
    bad = frame(0); bad.data[2] = bad.buffer.get() + bad.buffer_size;
    require(enc.encodeVideoFramePackets(bad).empty(), "accepted out-of-bounds plane");
    bad = frame(0); bad.buffer_size = 4;
    require(enc.encodeVideoFramePackets(bad).empty(), "accepted truncated allocation");
    bad = frame(0); bad.time_base = {0, 1};
    require(enc.encodeVideoFramePackets(bad).empty(), "accepted invalid clock");
    bad = frame(0); bad.pts = AV_NOPTS_VALUE;
    require(enc.encodeVideoFramePackets(bad).empty(), "accepted missing input timestamp");
    bad = frame(0); bad.buffer = std::shared_ptr<uint8_t>(bad.buffer,bad.buffer.get()+1);
    bad.data[0]=bad.data[1]=bad.data[2]=nullptr;
    require(enc.encodeVideoFramePackets(bad).empty(), "accepted sample-misaligned contiguous input");
    auto valid = frame(4);
    valid.time_base = {1, 100};
    auto packets = enc.encodeVideoFramePackets(valid);
    auto tail = enc.flush();
    for (auto& pkt : tail) packets.emplace_back(std::move(pkt));
    require(packets.size() == 1 && packets[0]->pts == 2, "timestamp was not rescaled");
}

static void packetsStayOwnedAndPresetDefaultsStayIntact()
{
    auto config=preset(true);
    config["preset"]="superfast"; config.erase("tune"); config.erase("additional_options");
    EncoderX265 encoder(config);
    std::ostringstream options; auto* original=std::cerr.rdbuf(options.rdbuf());
    const bool opened=encoder.initialize(); std::cerr.rdbuf(original);
    require(opened,"preset-only superfast failed");
    require(options.str().find("rc-lookahead=")==std::string::npos &&
            options.str().find(":rd=")==std::string::npos &&
            options.str().find(":ref=")==std::string::npos &&
            options.str().find("frame-threads=")==std::string::npos &&
            options.str().find("pools=")==std::string::npos,
            "wrapper added unrequested speed/thread overrides");
    std::vector<AVPacketPtr> held;
    std::vector<std::vector<uint8_t>> payloads;
    auto hold=[&](std::vector<AVPacketPtr> packets) {
        for(auto& p:packets) {
            payloads.emplace_back(p->data,p->data+p->size);
            held.emplace_back(std::move(p));
        }
    };
    for(int i=0;i<48;++i) hold(encoder.encodeVideoFramePackets(frame(i)));
    hold(encoder.flush());
    require(held.size()==48,"scratch packet draining lost buffered frames");
    std::set<int64_t> pts;
    for(size_t i=0;i<held.size();++i) {
        require(pts.insert(held[i]->pts).second,"packet timestamp reused");
        require(held[i]->duration==1,"packet duration lost during move");
        require(size_t(held[i]->size)==payloads[i].size() &&
                !std::memcmp(held[i]->data,payloads[i].data(),payloads[i].size()),
                "scratch reuse changed a previously returned packet");
    }
    for(const char* alias:{"threads","frame-threads","frame_threads"}) {
        auto threaded=preset(true); threaded["additional_options"].erase("frame-threads");
        threaded["additional_options"][alias]=1;
        EncoderX265 one(threaded); require(one.initialize(),"frame-thread alias failed");
        require(one.getCodecContext()->thread_count==1,"frame-thread alias not applied to FFmpeg");
        auto packets=one.encodeVideoFramePackets(frame(0));
        auto tail=one.flush();
        require(packets.size()+tail.size()==1,"thread-alias output lost");
    }
}

static void hdrAndOptions()
{
    {
        auto config = preset();
        config["rate_control"] = "crf";
        config["crf"] = 22.5;
        EncoderX265 encoder(config);
        std::ostringstream options;
        auto* original = std::cerr.rdbuf(options.rdbuf());
        const bool opened = encoder.initialize();
        std::cerr.rdbuf(original);
        require(opened && options.str().find("crf=22.5") != std::string::npos,
                "fractional CRF was lost");
        auto packets = encoder.encodeVideoFramePackets(frame(0));
        auto tail = encoder.flush();
        require(packets.size() + tail.size() == 1, "CRF frame was lost");
    }
    for (const char* transfer : {"arib-std-b67", "smpte2084"}) {
        auto config = preset();
        config["profile"] = "main10";
        config["output"]["bit_depth"] = 10;
        config["color"] = {{"primaries", "bt2020"}, {"transfer", transfer},
                           {"matrix", "bt2020nc"}, {"range", "limited"}};
        const bool pq = std::string(transfer) == "smpte2084";
        if (pq) config["hdr10"] = {
            {"master_display", "G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(10000000,50)"},
            {"max_cll", "1000,400"}};
        config["additional_options"]["aq-strength"] = 0.5;
        config["additional_options"]["psy-rd"] = 0.25;
        config["additional_options"]["ctu"] = 32;
        config["additional_options"]["wpp"] = 0;
        config["additional_options"]["strict-cbr"] = 1;
        EncoderX265 encoder(config);
        std::ostringstream options;
        auto* original = std::cerr.rdbuf(options.rdbuf());
        const bool opened = encoder.initialize();
        std::cerr.rdbuf(original);
        require(opened, "HDR initialization failed");
        require(options.str().find("vbv-init=0.75") != std::string::npos &&
                options.str().find("aq-strength=0.5") != std::string::npos &&
                options.str().find("psy-rd=0.25") != std::string::npos &&
                options.str().find("ctu=32") != std::string::npos &&
                options.str().find("strict-cbr=1") != std::string::npos,
                "typed options were not forwarded");
        auto packets = encoder.encodeVideoFramePackets(frame(0));
        auto tail = encoder.flush();
        for (auto& pkt : tail) packets.emplace_back(std::move(pkt));
        require(packets.size() == 1, "HDR frame was lost");
        auto* decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC));
        decoder->thread_count = 1;
        require(avcodec_open2(decoder, decoder->codec, nullptr) == 0, "HDR decoder failed");
        AVFrame* decoded = av_frame_alloc();
        require(avcodec_send_packet(decoder, packets[0].get()) == 0, "HDR packet was invalid");
        require(avcodec_send_packet(decoder, nullptr) == 0, "HDR decoder flush failed");
        require(avcodec_receive_frame(decoder, decoded) == 0, "HDR frame failed to decode");
        require(decoded->format == AV_PIX_FMT_YUV420P10LE && decoded->color_primaries == AVCOL_PRI_BT2020 &&
                decoded->color_trc == (pq ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_ARIB_STD_B67),
                "incorrect HDR format or transfer");
        if (pq) {
            require(av_frame_get_side_data(decoded, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA),
                    "HDR10 mastering SEI is missing");
            auto* sd = av_frame_get_side_data(decoded, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
            require(sd && reinterpret_cast<const AVContentLightMetadata*>(sd->data)->MaxCLL == 1000 &&
                    reinterpret_cast<const AVContentLightMetadata*>(sd->data)->MaxFALL == 400,
                    "HDR10 content light SEI is missing or incorrect");
        }
        av_frame_free(&decoded);
        avcodec_free_context(&decoder);
    }
}


static void reusableOutputAndAssembly()
{
    for (const auto& mode : {"auto", "avx2", "avx512"}) {
        auto cfg=preset(true);
        cfg["additional_options"]["asm"]=mode;
        EncoderX265 enc(cfg);
        const int needed=std::string(mode)=="avx2" ? AV_CPU_FLAG_AVX2 :
                         std::string(mode)=="avx512" ? AV_CPU_FLAG_AVX512 : 0;
        if (needed && !(av_get_cpu_flags() & needed)) {
            require(!enc.initialize(), "unsupported assembly request accepted");
            continue;
        }
        require(enc.initialize(), "assembly mode did not initialize");
        std::vector<AVPacketPtr> packets;
        packets.reserve(8);
        const auto capacity=packets.capacity();
        auto* storage=packets.data();
        for (int i=0;i<8;++i) {
            enc.encodeVideoFramePackets(frame(i),packets);
            require(packets.size()==1 && packets[0]->pts==i, "reusable output lost/duplicated a packet");
            require(packets.capacity()==capacity && packets.data()==storage, "packet vector allocated again");
        }
        auto bad=frame(9); bad.pts=AV_NOPTS_VALUE;
        enc.encodeVideoFramePackets(bad,packets);
        require(packets.empty(), "invalid input left stale output");
        packets.clear(); enc.flush();
    }
}


static void qualityOverridesKeepAutomaticThreads()
{
    auto config=preset(true);
    config["preset"]="superfast";
    config.erase("tune");
    config["additional_options"]={
        {"rd",6}, {"aq-mode",2}, {"aq-strength",1.0}, {"sao",true}, {"rdoq_level",2}, {"rd_refine",true}, {"rskip",2},
        {"rskip_edge_threshold",20}, {"early_skip",true}, {"no-fast-intra",true},
        {"limit_refs",1}, {"limit_modes",false}, {"limit_tu",1}, {"max_merge",3},
        {"tu_intra_depth",2}, {"tu_inter_depth",2}, {"max_tu_size",16},
        {"selective_sao",3}, {"nr_intra",50}, {"nr_inter",50},
        {"cbqpoffs",-2}, {"crqpoffs",2}, {"signhide",true}, {"weightp",true},
        {"weightb",false}, {"cutree",false}, {"aq_motion",false},
        {"tskip",true}, {"tskip_fast","true"}, {"sao_non_deblock",false}};
    EncoderX265 encoder(config);
    require(encoder.initialize(), "quality overrides failed to initialize");
    auto* ctx=encoder.getCodecContext();
    require(ctx->thread_count==0, "quality overrides changed automatic threading");
    uint8_t* value=nullptr;
    require(av_opt_get(ctx->priv_data,"x265-params",0,&value)==0 && value,
            "could not inspect forwarded options");
    const std::string options(reinterpret_cast<char*>(value)); av_free(value);
    for (const auto& option:std::vector<std::string>{
        "rdoq-level=2", "rd-refine=1", "rskip=2", "rskip-edge-threshold=20",
        "early-skip=1", "fast-intra=0", "limit-refs=1", "limit-modes=0", "limit-tu=1",
        "max-merge=3", "tu-intra-depth=2", "tu-inter-depth=2", "max-tu-size=16",
        "selective-sao=3", "nr-intra=50", "nr-inter=50", "cbqpoffs=-2", "crqpoffs=2",
        "signhide=1", "weightp=1", "weightb=0", "cutree=0", "aq-motion=0",
        "tskip=1", "tskip-fast=1", "sao-non-deblock=0"})
        require(options.find(option)!=std::string::npos,"quality option not forwarded");
    require(options.find("frame-threads=")==std::string::npos && options.find("pools=")==std::string::npos &&
            options.find("rc-lookahead=")==std::string::npos, "unrequested thread/lookahead override");
    std::vector<AVPacketPtr> packets;
    for(int i=0;i<24;++i) {
        auto output=encoder.encodeVideoFramePackets(frame(i));
        for(auto& p:output) packets.push_back(std::move(p));
    }
    auto tail=encoder.flush();
    for(auto& p:tail) packets.push_back(std::move(p));
    require(packets.size()==24,"quality overrides lost pictures");
    auto* decoder=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC));
    require(decoder,"quality decoder allocation failed");
    decoder->thread_count=1;
    require(avcodec_open2(decoder,decoder->codec,nullptr)==0,"quality decoder did not open");
    auto* image=av_frame_alloc(); require(image,"quality decode frame allocation failed");
    int decoded=0;
    auto drain=[&] {
        int ret;
        while((ret=avcodec_receive_frame(decoder,image))==0) {
            require(image->width==128 && image->height==96 && image->format==AV_PIX_FMT_YUV422P10LE,
                    "quality overrides changed output format");
            require(image->pts==decoded++,"quality overrides changed picture order");
            av_frame_unref(image);
        }
        require(ret==AVERROR(EAGAIN) || ret==AVERROR_EOF,"quality stream decode failed");
    };
    for(const auto& packet:packets) {
        require(avcodec_send_packet(decoder,packet.get())==0,"quality stream packet rejected"); drain();
    }
    require(avcodec_send_packet(decoder,nullptr)==0,"quality decoder drain failed"); drain();
    require(decoded==24,"quality stream lost decoded pictures");
    av_frame_free(&image); avcodec_free_context(&decoder);

    auto defaults=preset(true); defaults.erase("additional_options"); defaults.erase("tune");
    defaults["preset"]="superfast";
    EncoderX265 untouched(defaults); require(untouched.initialize(),"unchanged preset failed");
    value=nullptr;
    require(av_opt_get(untouched.getCodecContext()->priv_data,"x265-params",0,&value)==0 && value,
            "could not inspect default options");
    const std::string original(reinterpret_cast<char*>(value)); av_free(value);
    require(original.find("rdoq-level=")==std::string::npos && original.find("early-skip=")==std::string::npos &&
            original.find("cutree=")==std::string::npos && original.find("pools=")==std::string::npos &&
            original.find("frame-threads=")==std::string::npos,"omitted defaults were overridden");
    untouched.flush();
}


static void singleFrameContractRejectsBufferedSettings()
{
    auto reject=[](json value,const char* message) {
        EncoderX265 enc(value);
        require(!enc.initialize(),message);
    };

    auto value=preset(true); value["max_b_frames"]=1;
    reject(value,"single-frame accepted B frames");

    value=preset(true); value["additional_options"]["frame-threads"]=2;
    reject(value,"single-frame accepted frame-threads > 1");

    value=preset(true); value["additional_options"]["rc-lookahead"]=1;
    reject(value,"single-frame accepted lookahead");

    value=preset(true); value["additional_options"]["cutree"]=1;
    reject(value,"single-frame accepted cutree");
}

static void diagnosticCallsPreserveOutput() {
    const auto calls=[](const std::string& name) {
        for(const auto& sample:stage_timing::registry().snapshot()) if(sample.name==name) return sample.calls;
        return uint64_t(0);
    };
    auto run=[&](bool enabled) {
        stage_timing::set_enabled(enabled,false);
        EncoderX265 encoder(preset(true)); require(encoder.initialize(),"diagnostic encoder failed");
        std::vector<std::vector<uint8_t>> bytes;
        for(int i=0;i<16;++i) {
            auto output=encoder.encodeVideoFramePackets(frame(i));
            require(output.size()==1 && output[0]->pts==i,"diagnostics changed immediate output cadence");
            bytes.emplace_back(output[0]->data,output[0]->data+output[0]->size);
        }
        require(encoder.flush().empty(),"diagnostics changed flush output");
        return bytes;
    };
    const auto keyBefore=calls("x265_key_output_submit"),interBefore=calls("x265_inter_output_submit");
    const auto enabled=run(true);
    require(calls("x265_key_output_submit")-keyBefore==1,"key submit classification incorrect");
    require(calls("x265_inter_output_submit")-interBefore==15,"inter submit classification incorrect");
    const auto keyAfter=calls("x265_key_output_submit"),interAfter=calls("x265_inter_output_submit");
    const auto disabled=run(false);
    require(calls("x265_key_output_submit")==keyAfter && calls("x265_inter_output_submit")==interAfter,
            "disabled diagnostics recorded samples");
    require(enabled==disabled,"diagnostics changed encoded bytes");
}

int main(int argc, char** argv)
{
    if (!avcodec_find_encoder_by_name("libx265")) {
        std::cerr << "SKIP: FFmpeg has no libx265 encoder\n";
        return 77;
    }
    try {
        singleFrameContractRejectsBufferedSettings();
        diagnosticCallsPreserveOutput();
        invalidPresets();
        qualityOverridesKeepAutomaticThreads();
        reusableOutputAndAssembly();
        invalidFramesAndClock();
        for (bool native : {false, true})
            for (bool delayed : {false, true}) roundTrip(native, delayed);
        roundTrip(true, true, true);
        roundTrip(true, true, true, true);
        roundTrip(false, true, true, true);
        hdrAndOptions();
        packetsStayOwnedAndPresetDefaultsStayIntact();
        // Optionally validate actual presets using full configured resolution.
        for (int i = 1; i < argc; ++i) {
            std::ifstream input(argv[i]);
            json value; input >> value;
            EncoderX265 encoder(value);
            if (value.value("interlaced", false))
                require(!encoder.initialize(), "woven interlaced preset was accepted");
            else {
                require(encoder.initialize(), "production preset did not open");
                encoder.flush();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "PASS: x265 validation, ownership, conversion, captions, IDR, lookahead and flush\n";
}
