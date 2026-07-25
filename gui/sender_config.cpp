/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/sender_config.h"

#include "config/preset_validator.h"
#include "gui/mpegts_rate.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

struct TemplateRecord {
    std::string id;
    std::string path;
    json preset;
    json metadata;
};

bool regularFileExists(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string joinPath(const std::string& left, const std::string& right)
{
    if (left.empty()) return right;
    return left.back() == '/' ? left + right : left + "/" + right;
}

bool hasJsonExtension(const std::string& name)
{
    return name.size() > 5 && name.substr(name.size() - 5) == ".json";
}

std::string fileStem(const std::string& name)
{
    return hasJsonExtension(name) ? name.substr(0, name.size() - 5) : name;
}

bool isSafeId(const std::string& value)
{
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

bool isValidChannel(const std::string& value)
{
    return value == "sdi1" || value == "sdi2" || value == "sdi3" || value == "sdi4";
}

std::string humanize(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    bool capitalize = true;
    for (char c : value) {
        if (c == '_' || c == '-') {
            out.push_back(' ');
            capitalize = true;
            continue;
        }
        out.push_back(capitalize ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        capitalize = false;
    }
    return out;
}

bool readJsonFile(const std::string& path, json& value, std::string* error)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        if (error) *error = "cannot open JSON file '" + path + "'";
        return false;
    }
    try {
        input >> value;
    } catch (const std::exception& ex) {
        if (error) *error = "cannot parse '" + path + "': " + ex.what();
        return false;
    }
    if (!value.is_object()) {
        if (error) *error = "JSON root in '" + path + "' must be an object";
        return false;
    }
    return true;
}

bool ensureParentDirectories(const std::string& filePath, std::string* error)
{
    const std::size_t slash = filePath.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return true;

    const std::string parent = filePath.substr(0, slash);
    std::string current = (!parent.empty() && parent.front() == '/') ? "/" : std::string();
    std::size_t begin = (!parent.empty() && parent.front() == '/') ? 1 : 0;

    while (begin <= parent.size()) {
        const std::size_t end = parent.find('/', begin);
        const std::string part = parent.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!part.empty()) {
            if (current.size() > 1 && current.back() != '/') current += '/';
            current += part;
            struct stat st{};
            if (::stat(current.c_str(), &st) != 0) {
                if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    if (error) *error = "cannot create directory '" + current + "': " + std::strerror(errno);
                    return false;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                if (error) *error = "path component is not a directory: '" + current + "'";
                return false;
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

bool atomicWriteJson(const std::string& path, const json& value, std::string* error)
{
    if (!ensureParentDirectories(path, error)) return false;
    const std::string temporary = path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    {
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            if (error) *error = "cannot open temporary file for writing";
            return false;
        }
        output << value.dump(2) << '\n';
        output.flush();
        if (!output.good()) {
            if (error) *error = "failed while writing temporary channel preset";
            output.close();
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        if (error) *error = "cannot replace channel preset: " + std::string(std::strerror(errno));
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

json metadataForPreset(const json& preset, const std::string& fallbackId)
{
    json metadata = json::object();
    if (preset.contains("_gui") && preset["_gui"].is_object()) {
        metadata = preset["_gui"];
    }
    const std::string id = metadata.value("id", fallbackId);
    metadata["id"] = isSafeId(id) ? id : fallbackId;
    if (!metadata.contains("name") || !metadata["name"].is_string() || metadata["name"].get<std::string>().empty()) {
        metadata["name"] = humanize(metadata["id"].get<std::string>());
    }
    if (!metadata.contains("description") || !metadata["description"].is_string()) {
        metadata["description"] = "NxFrame sender template";
    }
    if (!metadata.contains("version") || !metadata["version"].is_number_integer()) {
        metadata["version"] = 1;
    }
    return metadata;
}

std::vector<TemplateRecord> scanTemplates(const std::string& root, std::vector<std::string>* warnings)
{
    std::vector<TemplateRecord> records;
    DIR* dir = ::opendir(root.c_str());
    if (!dir) {
        if (warnings) warnings->push_back("cannot open GUI encoder preset directory '" + root + "'");
        return records;
    }

    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || !hasJsonExtension(name)) continue;
        const std::string path = joinPath(root, name);
        if (!regularFileExists(path)) continue;

        json preset;
        std::string error;
        if (!readJsonFile(path, preset, &error)) {
            if (warnings) warnings->push_back(error);
            continue;
        }
        const PresetValidator::Result validation = PresetValidator::validateJson(preset, PresetValidator::Kind::Sender);
        if (!validation.ok()) {
            std::ostringstream message;
            message << "template '" << name << "' failed validation";
            for (const auto& item : validation.errors) message << "; " << item;
            if (warnings) warnings->push_back(message.str());
            continue;
        }

        TemplateRecord record;
        record.path = path;
        record.preset = std::move(preset);
        record.metadata = metadataForPreset(record.preset, fileStem(name));
        record.id = record.metadata.value("id", fileStem(name));
        if (!isSafeId(record.id)) {
            if (warnings) warnings->push_back("template '" + name + "' has an unsafe GUI id");
            continue;
        }
        records.push_back(std::move(record));
    }
    ::closedir(dir);

    std::sort(records.begin(), records.end(), [](const TemplateRecord& a, const TemplateRecord& b) {
        const int ao = a.metadata.value("order", 1000);
        const int bo = b.metadata.value("order", 1000);
        if (ao != bo) return ao < bo;
        return a.metadata.value("name", a.id) < b.metadata.value("name", b.id);
    });
    return records;
}

const json& requireObject(const json& parent, const char* key)
{
    if (!parent.contains(key) || !parent[key].is_object()) {
        throw std::runtime_error(std::string(key) + " must be an object");
    }
    return parent[key];
}

std::string requireString(const json& parent,
                          const char* key,
                          const std::string& path,
                          std::size_t minimum,
                          std::size_t maximum)
{
    if (!parent.contains(key) || !parent[key].is_string()) {
        throw std::runtime_error(path + "." + key + " must be a string");
    }
    const std::string value = parent[key].get<std::string>();
    if (value.size() < minimum || value.size() > maximum) {
        throw std::runtime_error(path + "." + key + " length must be " +
                                 std::to_string(minimum) + ".." + std::to_string(maximum));
    }
    return value;
}

std::string optionalString(const json& parent,
                           const char* key,
                           const std::string& fallback,
                           std::size_t maximum,
                           const std::string& path)
{
    if (!parent.contains(key)) return fallback;
    if (!parent[key].is_string()) throw std::runtime_error(path + "." + key + " must be a string");
    const std::string value = parent[key].get<std::string>();
    if (value.size() > maximum) throw std::runtime_error(path + "." + key + " is too long");
    return value;
}

int requireInt(const json& parent, const char* key, const std::string& path, int minimum, int maximum)
{
    if (!parent.contains(key) || !parent[key].is_number_integer()) {
        throw std::runtime_error(path + "." + key + " must be an integer");
    }
    const int value = parent[key].get<int>();
    if (value < minimum || value > maximum) {
        throw std::runtime_error(path + "." + key + " must be between " +
                                 std::to_string(minimum) + " and " + std::to_string(maximum));
    }
    return value;
}

bool requireBool(const json& parent, const char* key, const std::string& path)
{
    if (!parent.contains(key) || !parent[key].is_boolean()) {
        throw std::runtime_error(path + "." + key + " must be true or false");
    }
    return parent[key].get<bool>();
}

std::string requireEnum(const json& parent,
                        const char* key,
                        const std::string& path,
                        const std::set<std::string>& allowed)
{
    const std::string value = requireString(parent, key, path, 1, 64);
    if (!allowed.count(value)) {
        throw std::runtime_error(path + "." + key + " has unsupported value '" + value + "'");
    }
    return value;
}

struct VideoFormatSpec {
    const char* id;
    int width;
    int height;
    int framerate;
    bool interlaced;
    const char* fieldOrder;
};

static const VideoFormatSpec kVideoFormats[] = {
    {"720p50",  1280,  720, 50, false, ""},
    {"720p60",  1280,  720, 60, false, ""},
    {"1080i50", 1920, 1080, 25, true,  "tff"},
    {"1080i60", 1920, 1080, 30, true,  "tff"},
    {"1080p25", 1920, 1080, 25, false, ""},
    {"1080p30", 1920, 1080, 30, false, ""},
    {"1080p50", 1920, 1080, 50, false, ""},
    {"1080p60", 1920, 1080, 60, false, ""}
};

const VideoFormatSpec& videoFormatById(const std::string& id)
{
    for (const auto& format : kVideoFormats) {
        if (id == format.id) return format;
    }
    throw std::runtime_error("settings.video.format has unsupported value '" + id + "'");
}

std::string videoFormatFromPreset(const json& preset)
{
    const int width = preset.value("width", 1920);
    const int height = preset.value("height", 1080);
    const int framerate = preset.value("framerate", 25);
    const bool interlaced = preset.value("interlaced", false);
    for (const auto& format : kVideoFormats) {
        if (width == format.width && height == format.height &&
            framerate == format.framerate && interlaced == format.interlaced) {
            return format.id;
        }
    }
    return "1080i50";
}

int requireAacBitrate(const json& parent, const char* key, const std::string& path)
{
    const int bitrate = requireInt(parent, key, path, 64000, 384000);
    static const std::set<int> allowed = {
        64000, 96000, 128000, 160000, 192000, 224000, 256000, 320000, 384000
    };
    if (!allowed.count(bitrate)) {
        throw std::runtime_error(path + "." + key + " must be one of 64, 96, 128, 160, 192, 224, 256, 320, or 384 kbps");
    }
    return bitrate;
}

std::string deriveX264Profile(const std::string& chroma, int bitDepth)
{
    if (chroma == "420") return bitDepth == 10 ? "high10" : "high";
    if (chroma == "422") return "high422";
    return "high444";
}

std::string deriveX265Profile(const std::string& chroma, int bitDepth)
{
    if (chroma == "420") return bitDepth == 10 ? "main10" : "main";
    if (chroma == "422") return "main422-10";
    return "main444-10";
}

std::string x264ProfileLabel(const std::string& profile)
{
    if (profile == "baseline") return "Baseline";
    if (profile == "main") return "Main";
    if (profile == "high") return "High";
    if (profile == "high10") return "High 10";
    if (profile == "high422") return "High 4:2:2";
    return profile;
}

std::string normalizeX264Profile(const std::string& requested,
                                 const std::string& chroma,
                                 int bitDepth,
                                 bool interlaced,
                                 std::vector<std::string>& corrections)
{
    const std::string automatic = deriveX264Profile(chroma, bitDepth);
    if (requested == "auto") return automatic;

    bool compatible = false;
    std::string corrected = automatic;
    if (chroma == "422") {
        compatible = requested == "high422";
    } else if (bitDepth == 10) {
        compatible = requested == "high10";
    } else {
        compatible = requested == "main" || requested == "high" ||
            (requested == "baseline" && !interlaced);
        if (requested == "baseline" && interlaced) corrected = "main";
    }

    if (compatible) return requested;
    corrections.push_back(
        "H.264 profile changed from " + x264ProfileLabel(requested) +
        " to " + x264ProfileLabel(corrected) +
        " for " + chroma + " chroma at " + std::to_string(bitDepth) + "-bit" +
        (interlaced ? " interlaced video" : " progressive video"));
    return corrected;
}

std::string deriveH264Level(int width,
                            int height,
                            int framerate,
                            int maximumBitrate,
                            const std::string& profile)
{
    struct LevelLimit {
        const char* name;
        int maxMbsPerSecond;
        int maxFrameMbs;
        int baseMaxBitrate;
    };
    static const LevelLimit levels[] = {
        {"3.0",   40500,   1620,  10000000},
        {"3.1",  108000,   3600,  14000000},
        {"3.2",  216000,   5120,  20000000},
        {"4.0",  245760,   8192,  20000000},
        {"4.1",  245760,   8192,  50000000},
        {"4.2",  522240,   8704,  50000000},
        {"5.0",  589824,  22080, 135000000},
        {"5.1",  983040,  36864, 240000000},
        {"5.2", 2073600,  36864, 240000000}
    };

    double bitrateFactor = 1.0;
    if (profile == "high") bitrateFactor = 1.25;
    else if (profile == "high10") bitrateFactor = 3.0;
    else if (profile == "high422" || profile == "high444") bitrateFactor = 4.0;

    const int frameMbs = ((width + 15) / 16) * ((height + 15) / 16);
    const long long mbsPerSecond = static_cast<long long>(frameMbs) * framerate;
    for (const auto& level : levels) {
        if (frameMbs <= level.maxFrameMbs &&
            mbsPerSecond <= level.maxMbsPerSecond &&
            maximumBitrate <= static_cast<int>(level.baseMaxBitrate * bitrateFactor)) {
            return level.name;
        }
    }
    throw std::runtime_error("video format/bitrate exceeds the supported automatic H.264 level table");
}

int h264LevelRank(const std::string& level)
{
    static const std::vector<std::string> levels = {
        "3.0", "3.1", "3.2", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2"
    };
    const auto found = std::find(levels.begin(), levels.end(), level);
    return found == levels.end() ? -1 : static_cast<int>(std::distance(levels.begin(), found));
}

long long roundUp(long long value, long long step)
{
    if (step <= 0) return value;
    return ((value + step - 1) / step) * step;
}

long long estimatedPairBitrate(const json& pair, int aacBitrate, int sampleRate)
{
    const std::string type = pair.value("type", std::string());
    if (type == "aac_lc_mpeg2" || type == "aac_lc_mpeg4" || type == "aac" || type == "aac_lc") {
        return pair.value("bitrate", aacBitrate);
    }
    if (type == "pcm" || type == "s302m" || type == "dolby_e") {
        const int bits = pair.value("bits_per_raw_sample", 20);
        const long long raw = static_cast<long long>(sampleRate) * 2LL * bits;
        return roundUp(static_cast<long long>(std::ceil(raw * 1.20)), 1000);
    }
    return 0;
}

json editableFromPreset(const json& preset, const json& metadata)
{
    const json output = preset.contains("output") && preset["output"].is_object()
        ? preset["output"] : json::object();
    const json options = preset.contains("additional_options") && preset["additional_options"].is_object()
        ? preset["additional_options"] : json::object();
    const json mpegts = preset.contains("mpegts") && preset["mpegts"].is_object()
        ? preset["mpegts"] : json::object();
    const json audio = preset.contains("audio") && preset["audio"].is_object()
        ? preset["audio"] : json::object();

    json editable;
    editable["video"] = {
        {"format", videoFormatFromPreset(preset)},
        {"field_order", preset.value("interlaced", false)
            ? preset.value("field_order", std::string("tff"))
            : std::string()},
        {"bitrate", preset.value("bitrate", 15000000)},
        {"rate_control", options.value("rate_control", std::string("cbr"))},
        {"bit_depth", output.value("bit_depth", 10)},
        {"chroma", output.value("chroma", std::string("422"))},
        {"profile", metadata.value("h264_profile", std::string("auto"))},
        {"level", metadata.value("h264_level", std::string("auto"))}
    };

    editable["mpegts"] = {
        {"service_provider", mpegts.value("service_provider", std::string("NxFrame"))},
        {"service_name", mpegts.value("service_name", metadata.value("name", std::string("NxFrame")))},
        {"constant_rate", mpegts.value("null_stuffing", false)},
        {"auto_muxrate", metadata.value("mpegts_auto", true)},
        {"muxrate", mpegts.value("muxrate", 0)}
    };

    const int inputChannels = audio.value("input_channels", 2);
    const int pairCount = std::max(1, inputChannels / 2);
    json pairs = json::array();
    for (int i = 0; i < pairCount; ++i) {
        pairs.push_back({
            {"name", "Pair " + std::to_string(i + 1)},
            {"channels", json::array({i * 2 + 1, i * 2 + 2})},
            {"codec", "disabled"}
        });
    }

    // The runnable preset contains only enabled pair legs. Reconstruct their
    // natural GUI slots from the 1-based SDI channel map so disabling Pair 1
    // does not make Pair 2 reappear as the first row after a reload.
    if (audio.contains("pairs") && audio["pairs"].is_array()) {
        std::set<int> occupiedSlots;
        for (const auto& source : audio["pairs"]) {
            if (!source.is_object()) continue;

            int slot = -1;
            if (source.contains("channels") && source["channels"].is_array() &&
                source["channels"].size() == 2 &&
                source["channels"][0].is_number_integer() &&
                source["channels"][1].is_number_integer()) {
                const int left = source["channels"][0].get<int>();
                const int right = source["channels"][1].get<int>();
                if (left >= 1 && right == left + 1 && left % 2 == 1) {
                    const int candidate = (left - 1) / 2;
                    if (candidate >= 0 && candidate < pairCount) slot = candidate;
                }
            }
            if (slot < 0 || occupiedSlots.count(slot)) {
                for (int candidate = 0; candidate < pairCount; ++candidate) {
                    if (!occupiedSlots.count(candidate)) {
                        slot = candidate;
                        break;
                    }
                }
            }
            if (slot < 0 || slot >= pairCount) continue;
            occupiedSlots.insert(slot);

            json& pair = pairs[static_cast<std::size_t>(slot)];
            pair["name"] = source.value("name", pair["name"].get<std::string>());
            if (source.contains("channels") && source["channels"].is_array() && source["channels"].size() == 2) {
                pair["channels"] = source["channels"];
            }
            pair["codec"] = source.value("type", audio.value("codec", std::string("aac_lc_mpeg4")));
            pair["standard"] = source.value("standard", audio.value("standard", std::string("mpeg4")));
            pair["bitrate"] = source.value("bitrate", audio.value("bitrate", 192000));
            pair["profile"] = source.value("profile", audio.value("profile", std::string("aac_low")));
            pair["transport"] = source.value("transport", audio.value("transport", std::string("adts")));
        }
    }

    std::string stereoCodec = audio.value("codec", std::string("aac_lc_mpeg4"));
    if (audio.value("layout", std::string("split_pairs")) != "split_pairs" &&
        audio.contains("passthrough_pairs") && audio["passthrough_pairs"].is_array() &&
        !audio["passthrough_pairs"].empty() && audio["passthrough_pairs"][0].is_object()) {
        stereoCodec = audio["passthrough_pairs"][0].value("type", stereoCodec);
    }

    editable["audio"] = {
        {"split_pairs", audio.value("layout", std::string("split_pairs")) == "split_pairs"},
        {"input_channels", inputChannels},
        {"sample_rate", audio.value("sample_rate", 48000)},
        {"bitrate", audio.value("bitrate", 192000)},
        {"profile", audio.value("profile", std::string("aac_low"))},
        {"transport", audio.value("transport", std::string("adts"))},
        {"stereo_codec", stereoCodec},
        {"stereo_bitrate", audio.value("bitrate", 192000)},
        {"stereo_profile", audio.value("profile", std::string("aac_low"))},
        {"stereo_transport", audio.value("transport", std::string("adts"))},
        {"pairs", pairs}
    };

    std::string protocol = metadata.value(
        "protocol", metadata.value("default_protocol", std::string()));
    if (protocol.empty()) {
        if (preset.contains("srt") && preset["srt"].is_object()) protocol = "srt";
        else if (preset.contains("udp") && preset["udp"].is_object()) protocol = "udp";
        else protocol = "srt";
    }

    json streaming = {
        {"protocol", protocol},
        {"address", protocol == "srt" ? "0.0.0.0" : "239.1.1.1"},
        {"port", 5000},
        {"interface", ""}
    };
    if (protocol == "srt" && preset.contains("srt") && preset["srt"].is_object()) {
        const json& srt = preset["srt"];
        const std::string mode = srt.value("mode", std::string("listener"));
        streaming["mode"] = mode;
        streaming["address"] = srt.value("address", srt.value("bind_address", mode == "listener" ? "0.0.0.0" : ""));
        streaming["port"] = srt.value("port", 5000);
        streaming["latency"] = srt.value("latency", 120);
        streaming["streamid"] = srt.value("streamid", std::string());
        streaming["passphrase"] = srt.value("passphrase", std::string());
        streaming["pbkeylen"] = srt.value("pbkeylen", 0);
    } else if (preset.contains("udp") && preset["udp"].is_object()) {
        const json& udp = preset["udp"];
        streaming["address"] = udp.value("address", std::string("239.1.1.1"));
        streaming["port"] = udp.value("port", 5000);
        streaming["interface"] = udp.value("interface", std::string());
        streaming["ttl"] = udp.value("ttl", udp.value("multicast_ttl", 16));
    }
    editable["streaming"] = streaming;
    return editable;
}


} // namespace

SenderConfigStore::SenderConfigStore(std::string templateRoot, std::string channelRoot)
    : templateRoot_(std::move(templateRoot)),
      channelRoot_(std::move(channelRoot))
{
}

std::string SenderConfigStore::channelPath(const std::string& channel) const
{
    return isValidChannel(channel) ? joinPath(channelRoot_, channel + ".json") : std::string();
}

nlohmann::json SenderConfigStore::listTemplates(std::string* warning) const
{
    std::vector<std::string> warnings;
    const std::vector<TemplateRecord> records = scanTemplates(templateRoot_, &warnings);
    json result = json::array();
    for (const auto& record : records) {
        result.push_back({
            {"id", record.id},
            {"name", record.metadata.value("name", humanize(record.id))},
            {"description", record.metadata.value("description", std::string())},
            {"version", record.metadata.value("version", 1)},
            {"editable", editableFromPreset(record.preset, record.metadata)}
        });
    }
    if (warning && !warnings.empty()) {
        std::ostringstream joined;
        for (std::size_t i = 0; i < warnings.size(); ++i) {
            if (i) joined << " | ";
            joined << warnings[i];
        }
        *warning = joined.str();
    }
    return result;
}

bool SenderConfigStore::loadTemplate(const std::string& templateId,
                                     nlohmann::json& preset,
                                     nlohmann::json* metadata,
                                     std::string* error) const
{
    if (!isSafeId(templateId)) {
        if (error) *error = "invalid template id";
        return false;
    }
    std::vector<std::string> warnings;
    const std::vector<TemplateRecord> records = scanTemplates(templateRoot_, &warnings);
    for (const auto& record : records) {
        if (record.id == templateId) {
            preset = record.preset;
            if (metadata) *metadata = record.metadata;
            return true;
        }
    }
    if (error) *error = "GUI encoder template '" + templateId + "' was not found or is invalid";
    return false;
}

nlohmann::json SenderConfigStore::loadChannel(const std::string& channel, std::string* warning) const
{
    if (!isValidChannel(channel)) {
        if (warning) *warning = "invalid SDI channel";
        return {{"exists", false}};
    }
    const std::string path = channelPath(channel);
    if (!regularFileExists(path)) {
        return {{"exists", false}};
    }

    json preset;
    std::string error;
    if (!readJsonFile(path, preset, &error)) {
        if (warning) *warning = error;
        return {{"exists", false}};
    }
    const PresetValidator::Result validation = PresetValidator::validateJson(preset, PresetValidator::Kind::Sender);
    if (!validation.ok()) {
        if (warning) {
            std::ostringstream message;
            message << "saved channel preset failed validation";
            for (const auto& item : validation.errors) message << "; " << item;
            *warning = message.str();
        }
        return {{"exists", false}};
    }

    const json metadata = preset.value("_gui", json::object());
    return {
        {"exists", true},
        {"template_id", metadata.value("template_id", std::string())},
        {"configuration_name", metadata.value("configuration_name", preset.value("mpegts", json::object()).value("service_name", channel))},
        {"settings", editableFromPreset(preset, metadata)}
    };
}

bool SenderConfigStore::buildChannelPreset(const std::string& channel,
                                           const nlohmann::json& request,
                                           nlohmann::json& preset,
                                           nlohmann::json& response,
                                           std::string* error) const
{
    try {
        if (!isValidChannel(channel)) throw std::runtime_error("channel must be sdi1, sdi2, sdi3, or sdi4");
        if (!request.is_object()) throw std::runtime_error("request root must be an object");

        const std::string templateId = requireString(request, "template_id", "request", 1, 64);
        json metadata;
        if (!loadTemplate(templateId, preset, &metadata, error)) return false;
        preset.erase("_gui");

        std::string configurationName = optionalString(
            request, "configuration_name", metadata.value("name", humanize(channel)), 96, "request");
        if (configurationName.empty()) {
            configurationName = metadata.value("name", humanize(channel));
        }
        const json& settings = requireObject(request, "settings");

        // ---------------- Video ----------------
        const json& video = requireObject(settings, "video");
        const std::string formatId = requireEnum(
            video, "format", "settings.video",
            {"720p50", "720p60", "1080i50", "1080i60", "1080p25", "1080p30", "1080p50", "1080p60"});
        const VideoFormatSpec& format = videoFormatById(formatId);
        const int width = format.width;
        const int height = format.height;
        const int framerate = format.framerate;
        const std::string fieldOrder = format.interlaced
            ? requireEnum(video, "field_order", "settings.video", {"tff", "bff"})
            : std::string();
        const int bitrate = requireInt(video, "bitrate", "settings.video", 500000, 300000000);
        const std::string rateControl = requireEnum(video, "rate_control", "settings.video", {"cbr", "vbr"});
        const int bitDepth = requireInt(video, "bit_depth", "settings.video", 8, 10);
        if (!(bitDepth == 8 || bitDepth == 10)) throw std::runtime_error("settings.video.bit_depth must be 8 or 10");
        const std::string chroma = requireEnum(video, "chroma", "settings.video", {"420", "422"});
        const std::string profileSelection = video.contains("profile")
            ? requireEnum(video, "profile", "settings.video",
                          {"auto", "baseline", "main", "high", "high10", "high422"})
            : std::string("auto");
        const std::string levelSelection = video.contains("level")
            ? requireEnum(video, "level", "settings.video",
                          {"auto", "3.0", "3.1", "3.2", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2"})
            : std::string("auto");

        std::vector<std::string> corrections;
        const std::string codec = preset.value("codec", std::string("x264"));
        std::string profile;
        if (codec == "x264" || codec == "h264") {
            profile = normalizeX264Profile(profileSelection, chroma, bitDepth, format.interlaced, corrections);
        } else {
            if (profileSelection != "auto") {
                throw std::runtime_error("manual H.264 profile is available only for x264 templates");
            }
            profile = deriveX265Profile(chroma, bitDepth);
        }
        const double vbrMultiplier = metadata.value("vbr_maxrate_multiplier", 1.25);
        const double bufferSeconds = metadata.value("vbv_buffer_seconds", 0.5);
        if (vbrMultiplier < 1.0 || vbrMultiplier > 2.0) {
            throw std::runtime_error("template vbr_maxrate_multiplier must be between 1.0 and 2.0");
        }
        if (bufferSeconds < 0.1 || bufferSeconds > 5.0) {
            throw std::runtime_error("template vbv_buffer_seconds must be between 0.1 and 5.0");
        }
        const int maximumBitrate = rateControl == "cbr"
            ? bitrate
            : static_cast<int>(roundUp(static_cast<long long>(std::ceil(bitrate * vbrMultiplier)), 1000));
        const int vbvBuffer = static_cast<int>(roundUp(
            static_cast<long long>(std::ceil(maximumBitrate * bufferSeconds)), 1000));

        preset["width"] = width;
        preset["height"] = height;
        preset["framerate"] = framerate;
        preset["bitrate"] = bitrate;
        preset["vbv-maxrate"] = maximumBitrate;
        preset["vbv_bufsize"] = vbvBuffer;
        preset["profile"] = profile;
        preset["interlaced"] = format.interlaced;
        if (format.interlaced) preset["field_order"] = fieldOrder;
        else preset.erase("field_order");
        preset["output"] = preset.value("output", json::object());
        preset["output"]["bit_depth"] = bitDepth;
        preset["output"]["chroma"] = chroma;
        preset["additional_options"] = preset.value("additional_options", json::object());
        preset["additional_options"]["rate_control"] = rateControl;
        preset["additional_options"]["nal-hrd"] = rateControl;
        preset["additional_options"]["filler"] = rateControl == "cbr" ? 1 : 0;
        std::string storedLevelSelection = levelSelection;
        if (codec == "x264" || codec == "h264") {
            const std::string requiredLevel = deriveH264Level(width, height, framerate, maximumBitrate, profile);
            if (levelSelection != "auto" && h264LevelRank(levelSelection) < h264LevelRank(requiredLevel)) {
                corrections.push_back(
                    "H.264 level raised from " + levelSelection + " to " + requiredLevel +
                    " for the selected format, bitrate, and profile");
                storedLevelSelection = requiredLevel;
            }
            preset["additional_options"]["level"] =
                storedLevelSelection == "auto" ? requiredLevel : storedLevelSelection;
        } else if (levelSelection != "auto") {
            throw std::runtime_error("manual H.264 level is available only for x264 templates");
        }

        // ---------------- Audio ----------------
        const json& audioRequest = requireObject(settings, "audio");
        const bool splitPairs = requireBool(audioRequest, "split_pairs", "settings.audio");
        int inputChannels = requireInt(audioRequest, "input_channels", "settings.audio", 2, 16);
        if (inputChannels % 2 != 0) throw std::runtime_error("settings.audio.input_channels must be even");
        if (!splitPairs && inputChannels != 2) {
            throw std::runtime_error("single stereo stream mode requires 2 input channels");
        }
        const int sampleRate = requireInt(audioRequest, "sample_rate", "settings.audio", 8000, 192000);
        if (sampleRate != 48000) throw std::runtime_error("NxFrame SDI audio currently supports 48000 Hz only");
        json builtAudio = preset.value("audio", json::object());
        const int defaultAacBitrate = builtAudio.value("bitrate", 192000);
        const std::string defaultAacProfile = builtAudio.value("profile", std::string("aac_low"));
        const std::string defaultAacTransport = builtAudio.value("transport", std::string("adts"));
        builtAudio["layout"] = splitPairs ? "split_pairs" : "stereo";
        builtAudio["input_channels"] = inputChannels;
        builtAudio["sample_rate"] = sampleRate;
        builtAudio.erase("passthrough_pairs");
        builtAudio["pairs"] = json::array();

        long long estimatedAudioBitrate = 0;
        std::string firstCodec;
        std::string firstStandard = "mpeg4";

        if (splitPairs) {
            if (!audioRequest.contains("pairs") || !audioRequest["pairs"].is_array()) {
                throw std::runtime_error("settings.audio.pairs must be an array");
            }
            for (std::size_t i = 0; i < audioRequest["pairs"].size(); ++i) {
                const json& source = audioRequest["pairs"][i];
                if (!source.is_object()) throw std::runtime_error("settings.audio.pairs entries must be objects");
                const std::string codecChoice = requireEnum(
                    source, "codec", "settings.audio.pairs[" + std::to_string(i) + "]",
                    {"disabled", "aac_lc_mpeg4", "aac_lc_mpeg2", "s302m", "dolby_e"});
                if (codecChoice == "disabled") continue;

                const int left = static_cast<int>(i) * 2 + 1;
                const int right = left + 1;
                if (right > inputChannels) {
                    throw std::runtime_error("settings.audio.pairs contains more pairs than the selected input channel count");
                }

                json pair = {
                    {"name", "Pair " + std::to_string(i + 1)},
                    {"channels", json::array({left, right})},
                    {"type", codecChoice}
                };
                int pairAacBitrate = defaultAacBitrate;
                if (codecChoice == "aac_lc_mpeg4" || codecChoice == "aac_lc_mpeg2") {
                    pairAacBitrate = source.contains("bitrate")
                        ? requireAacBitrate(source, "bitrate", "settings.audio.pairs[" + std::to_string(i) + "]")
                        : defaultAacBitrate;
                    const std::string pairProfile = source.contains("profile")
                        ? requireEnum(source, "profile", "settings.audio.pairs[" + std::to_string(i) + "]", {"aac_low"})
                        : defaultAacProfile;
                    const std::string pairTransport = source.contains("transport")
                        ? requireEnum(source, "transport", "settings.audio.pairs[" + std::to_string(i) + "]", {"adts"})
                        : defaultAacTransport;
                    pair["standard"] = codecChoice == "aac_lc_mpeg2" ? "mpeg2" : "mpeg4";
                    pair["bitrate"] = pairAacBitrate;
                    pair["profile"] = pairProfile;
                    pair["transport"] = pairTransport;
                } else if (codecChoice == "dolby_e") {
                    pair["bits_per_raw_sample"] = 20;
                } else if (codecChoice == "s302m") {
                    pair["bits_per_raw_sample"] = 20;
                }
                if (firstCodec.empty()) {
                    firstCodec = codecChoice;
                    firstStandard = pair.value("standard", std::string("mpeg4"));
                }
                estimatedAudioBitrate += estimatedPairBitrate(pair, pairAacBitrate, sampleRate);
                builtAudio["pairs"].push_back(pair);
            }
            if (builtAudio["pairs"].empty()) {
                throw std::runtime_error("enable at least one audio pair");
            }
        } else {
            const std::string codecChoice = requireEnum(
                audioRequest, "stereo_codec", "settings.audio",
                {"aac_lc_mpeg4", "aac_lc_mpeg2", "s302m", "dolby_e"});
            builtAudio.erase("pairs");
            builtAudio.erase("passthrough_pairs");
            builtAudio["channels"] = 2;
            builtAudio["channel_map"] = json::array({1, 2});
            firstCodec = codecChoice;
            firstStandard = codecChoice == "aac_lc_mpeg2" ? "mpeg2" : "mpeg4";

            int stereoAacBitrate = defaultAacBitrate;
            json estimatePair = {{"type", codecChoice}};
            if (codecChoice == "aac_lc_mpeg4" || codecChoice == "aac_lc_mpeg2") {
                stereoAacBitrate = audioRequest.contains("stereo_bitrate")
                    ? requireAacBitrate(audioRequest, "stereo_bitrate", "settings.audio")
                    : defaultAacBitrate;
                const std::string stereoProfile = audioRequest.contains("stereo_profile")
                    ? requireEnum(audioRequest, "stereo_profile", "settings.audio", {"aac_low"})
                    : defaultAacProfile;
                const std::string stereoTransport = audioRequest.contains("stereo_transport")
                    ? requireEnum(audioRequest, "stereo_transport", "settings.audio", {"adts"})
                    : defaultAacTransport;
                builtAudio["bitrate"] = stereoAacBitrate;
                builtAudio["profile"] = stereoProfile;
                builtAudio["transport"] = stereoTransport;
                builtAudio.erase("bits_per_raw_sample");
                builtAudio["standard"] = firstStandard;
                estimatePair["bitrate"] = stereoAacBitrate;
            } else {
                builtAudio.erase("bitrate");
                builtAudio.erase("profile");
                builtAudio.erase("transport");
                builtAudio["bits_per_raw_sample"] = 20;
                estimatePair["bits_per_raw_sample"] = 20;
                if (codecChoice == "dolby_e") {
                    builtAudio["passthrough_pairs"] = json::array();
                    builtAudio["passthrough_pairs"].push_back({
                        {"type", "dolby_e"},
                        {"channels", json::array({1, 2})},
                        {"bits_per_raw_sample", 20}
                    });
                }
            }
            estimatedAudioBitrate = estimatedPairBitrate(estimatePair, stereoAacBitrate, sampleRate);
        }

        if (firstCodec == "s302m" || firstCodec == "dolby_e") {
            builtAudio["mode"] = "pcm";
            builtAudio["codec"] = "s302m";
        } else {
            builtAudio["mode"] = "encode";
            builtAudio["codec"] = firstCodec.empty() ? "aac_lc_mpeg4" : firstCodec;
        }
        builtAudio["standard"] = firstStandard;
        preset["audio"] = builtAudio;

        // ---------------- MPEG-TS ----------------
        const json& tsRequest = requireObject(settings, "mpegts");
        const std::string serviceProvider = requireString(tsRequest, "service_provider", "settings.mpegts", 1, 64);
        const std::string serviceName = requireString(tsRequest, "service_name", "settings.mpegts", 1, 96);
        const bool constantRate = requireBool(tsRequest, "constant_rate", "settings.mpegts");
        const bool autoMuxrate = tsRequest.contains("auto_muxrate")
            ? requireBool(tsRequest, "auto_muxrate", "settings.mpegts")
            : true;
        const int recommendedMuxrate = constantRate
            ? nxframe_gui::calculateRecommendedTsMuxrate(maximumBitrate, estimatedAudioBitrate)
            : 0;
        int muxrate = 0;
        if (constantRate) {
            if (autoMuxrate) {
                muxrate = recommendedMuxrate;
            } else {
                muxrate = requireInt(
                    tsRequest,
                    "muxrate",
                    "settings.mpegts",
                    100000,
                    static_cast<int>(nxframe_gui::kMaximumSupportedMuxrateBps));
                if (muxrate < recommendedMuxrate) {
                    std::ostringstream message;
                    message << "MPEG-TS rate must be at least "
                            << (recommendedMuxrate / 1000000.0)
                            << " Mbps for the selected video and audio settings";
                    throw std::runtime_error(message.str());
                }
            }
        }
        preset["mpegts"] = preset.value("mpegts", json::object());
        preset["mpegts"]["service_provider"] = serviceProvider;
        preset["mpegts"]["service_name"] = serviceName;
        preset["mpegts"]["muxrate"] = muxrate;
        preset["mpegts"]["null_stuffing"] = constantRate;

        // ---------------- Streaming transport ----------------
        const json& streaming = requireObject(settings, "streaming");
        const std::string protocol = requireEnum(streaming, "protocol", "settings.streaming", {"srt", "udp", "rtp"});
        std::string address = requireString(streaming, "address", "settings.streaming", 1, 255);
        const int port = requireInt(streaming, "port", "settings.streaming", 1, 65535);

        const json templateSrt = preset.contains("srt") && preset["srt"].is_object()
            ? preset["srt"] : json::object();
        const json templateUdp = preset.contains("udp") && preset["udp"].is_object()
            ? preset["udp"] : json::object();
        preset.erase("srt");
        preset.erase("udp");
        preset["streaming"] = {
            {"protocol", protocol},
            {"address", address},
            {"port", port}
        };

        if (protocol == "srt") {
            const std::string mode = requireEnum(streaming, "mode", "settings.streaming", {"listener", "caller", "rendezvous"});
            if (mode == "listener" && address == "*") address = "0.0.0.0";
            if ((mode == "caller" || mode == "rendezvous") && (address == "0.0.0.0" || address == "*")) {
                throw std::runtime_error("SRT caller/rendezvous mode requires a destination address");
            }
            const int latency = requireInt(streaming, "latency", "settings.streaming", 20, 30000);
            const std::string streamid = optionalString(streaming, "streamid", std::string(), 512, "settings.streaming");
            const std::string passphrase = optionalString(streaming, "passphrase", std::string(), 79, "settings.streaming");
            if (!passphrase.empty() && passphrase.size() < 10) {
                throw std::runtime_error("SRT passphrase must be empty or contain 10..79 characters");
            }
            int pbkeylen = 0;
            if (!passphrase.empty()) {
                pbkeylen = requireInt(streaming, "pbkeylen", "settings.streaming", 16, 32);
                if (!(pbkeylen == 16 || pbkeylen == 24 || pbkeylen == 32)) {
                    throw std::runtime_error("SRT encryption key length must be 16, 24, or 32 bytes");
                }
            }

            preset["streaming"]["address"] = address;
            json srt = templateSrt;
            srt["mode"] = mode;
            srt["address"] = address;
            srt["port"] = port;
            srt["latency"] = latency;
            srt["payload_size"] = srt.value("payload_size", 1316);
            srt["tlpktdrop"] = srt.value("tlpktdrop", true);
            srt["messageapi"] = srt.value("messageapi", true);
            srt["streamid"] = streamid;
            srt["passphrase"] = passphrase;
            srt["pbkeylen"] = pbkeylen;
            srt["maxbw"] = srt.value("maxbw", 0);
            srt["inputbw"] = constantRate ? muxrate : maximumBitrate;
            srt["oheadbw"] = srt.value("oheadbw", 25);
            srt["connect_timeout_ms"] = srt.value("connect_timeout_ms", 3000);
            srt["reconnect_attempts"] = srt.value("reconnect_attempts", 5);
            srt["reconnect_delay_ms"] = srt.value("reconnect_delay_ms", 1000);
            srt["send_timeout_ms"] = srt.value("send_timeout_ms", 1000);
            srt["recv_timeout_ms"] = srt.value("recv_timeout_ms", 1000);
            srt["sndbuf"] = srt.value("sndbuf", 16777216);
            srt["rcvbuf"] = srt.value("rcvbuf", 16777216);
            // Hidden template values remain intact; approved fields above are replaced.
            if (mode == "listener") srt["bind_address"] = address;
            else srt.erase("bind_address");
            preset["srt"] = srt;
            preset["streaming"]["mode"] = mode;
        } else {
            if (address == "0.0.0.0" || address == "*") {
                throw std::runtime_error("UDP/RTP requires a destination address");
            }
            const std::string interfaceName = optionalString(streaming, "interface", std::string(), 64, "settings.streaming");
            const int ttl = requireInt(streaming, "ttl", "settings.streaming", 0, 255);
            json udp = templateUdp;
            udp["address"] = address;
            udp["port"] = port;
            udp["interface"] = interfaceName;
            udp["payload_size"] = udp.value("payload_size", 1316);
            udp["ttl"] = ttl;
            udp["multicast_ttl"] = ttl;
            udp["rtp"] = protocol == "rtp";
            udp["pacing_enabled"] = constantRate || maximumBitrate > 0;
            udp["pacing_bitrate_bps"] = constantRate ? muxrate : maximumBitrate;
            preset["udp"] = udp;
            preset["streaming"]["interface"] = interfaceName;
            preset["streaming"]["ttl"] = ttl;
        }

        preset["_gui"] = {
            {"schema_version", 1},
            {"channel", channel},
            {"template_id", templateId},
            {"template_version", metadata.value("version", 1)},
            {"configuration_name", configurationName},
            {"protocol", protocol},
            {"mpegts_auto", autoMuxrate},
            {"h264_profile", profileSelection == "auto" ? std::string("auto") : profile},
            {"h264_level", storedLevelSelection}
        };

        const PresetValidator::Result validation = PresetValidator::validateJson(preset, PresetValidator::Kind::Sender);
        if (!validation.ok()) {
            std::ostringstream message;
            message << "generated sender preset failed validation";
            for (const auto& item : validation.errors) message << "; " << item;
            throw std::runtime_error(message.str());
        }

        response = {
            {"ok", true},
            {"channel", channel},
            {"template_id", templateId},
            {"configuration_name", configurationName},
            {"settings", editableFromPreset(preset, preset["_gui"])},
            {"warnings", validation.warnings},
            {"corrections", corrections}
        };
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

bool SenderConfigStore::validateChannelRequest(const std::string& channel,
                                               const nlohmann::json& request,
                                               nlohmann::json& response,
                                               std::string* error) const
{
    json preset;
    return buildChannelPreset(channel, request, preset, response, error);
}

bool SenderConfigStore::saveChannel(const std::string& channel,
                                    const nlohmann::json& request,
                                    nlohmann::json& response,
                                    std::string* error) const
{
    json preset;
    if (!buildChannelPreset(channel, request, preset, response, error)) return false;
    const std::string path = channelPath(channel);
    if (!atomicWriteJson(path, preset, error)) return false;
    response["message"] = "sender preset saved";
    return true;
}
