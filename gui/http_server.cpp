/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Description:
 * Small Linux HTTP server for the NxFrame appliance control panel. It serves
 * static dashboard assets and a deliberately narrow JSON API.
 */

#include "gui/http_server.h"

#include "gui/network_interfaces.h"
#include "gui/receiver_config.h"
#include "gui/sender_process_manager.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <cmath>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using json = nlohmann::json;
constexpr std::size_t kMaximumHeaderBytes = 32 * 1024;
constexpr std::size_t kMaximumBodyBytes = 1024 * 1024;

struct Request {
    std::string method;
    std::string path;
    std::string body;
};

std::string getHostname()
{
    char buffer[256]{};
    if (::gethostname(buffer, sizeof(buffer) - 1) == 0) return buffer;
    return "NxFrame";
}

bool regularFileExists(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Only read paths supplied by the process manager for this channel.
// Bound reads to the final 32 KiB so the API never streams an entire session log.
std::string logTail(const std::string& path)
{
    if (path.empty() || !regularFileExists(path)) return {};
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    if (size < 0) return {};
    constexpr std::streamoff limit = 32 * 1024;
    const std::streamoff start = size > limit ? static_cast<std::streamoff>(size) - limit : 0;
    input.seekg(start);
    std::string result(static_cast<std::size_t>(static_cast<std::streamoff>(size) - start), '\0');
    if (!result.empty()) input.read(&result[0], static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<std::size_t>(input.gcount()));
    // Do not expose common SRT URL secrets through the unauthenticated dashboard.
    for (const std::string key : {"passphrase=", "passphrase%3D"}) {
        std::size_t pos = 0;
        while ((pos = result.find(key, pos)) != std::string::npos) {
            const std::size_t begin = pos + key.size();
            const std::size_t end = result.find_first_of("& #\r\n", begin);
            result.replace(begin, (end == std::string::npos ? result.size() : end) - begin, "[REDACTED]");
            pos = begin + 10;
        }
    }
    return result;
}

std::string mimeType(const std::string& path)
{
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css; charset=utf-8";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") return "application/javascript; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".svg") return "image/svg+xml";
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") return "application/json; charset=utf-8";
    return "application/octet-stream";
}

bool sendAll(int fd, const std::string& data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void sendResponse(int fd,
                  int status,
                  const std::string& reason,
                  const std::string& contentType,
                  const std::string& body,
                  const std::string& extraHeaders = {})
{
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store\r\n"
             << "X-Content-Type-Options: nosniff\r\n"
             << "X-Frame-Options: DENY\r\n"
             << "Referrer-Policy: no-referrer\r\n"
             << "Content-Security-Policy: default-src 'self'; style-src 'self'; script-src 'self'; img-src 'self' data:; connect-src 'self'\r\n"
             << extraHeaders
             << "Connection: close\r\n\r\n"
             << body;
    sendAll(fd, response.str());
}

void sendJson(int fd, int status, const std::string& reason, const json& body)
{
    sendResponse(fd, status, reason, "application/json; charset=utf-8", body.dump());
}

bool parseContentLength(const std::string& headers, std::size_t& contentLength)
{
    std::istringstream stream(headers);
    std::string line;
    contentLength = 0;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == "content-length") {
            try {
                contentLength = static_cast<std::size_t>(std::stoull(line.substr(colon + 1)));
                return contentLength <= kMaximumBodyBytes;
            } catch (...) {
                return false;
            }
        }
    }
    return true;
}

bool readRequest(int fd, Request& request)
{
    std::string buffer;
    buffer.reserve(4096);
    char chunk[4096];
    std::size_t headerEnd = std::string::npos;

    while (headerEnd == std::string::npos) {
        const ssize_t count = ::recv(fd, chunk, sizeof(chunk), 0);
        if (count <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(count));
        if (buffer.size() > kMaximumHeaderBytes) return false;
        headerEnd = buffer.find("\r\n\r\n");
    }

    const std::string headers = buffer.substr(0, headerEnd + 2);
    std::istringstream firstLine(headers);
    std::string httpVersion;
    firstLine >> request.method >> request.path >> httpVersion;
    if (request.method.empty() || request.path.empty() || httpVersion.compare(0, 5, "HTTP/") != 0) return false;

    const std::size_t query = request.path.find('?');
    if (query != std::string::npos) request.path.resize(query);

    std::size_t contentLength = 0;
    if (!parseContentLength(headers, contentLength)) return false;
    const std::size_t bodyStart = headerEnd + 4;
    while (buffer.size() - bodyStart < contentLength) {
        const ssize_t count = ::recv(fd, chunk, sizeof(chunk), 0);
        if (count <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(count));
        if (buffer.size() - bodyStart > kMaximumBodyBytes) return false;
    }
    request.body = buffer.substr(bodyStart, contentLength);
    return true;
}

std::string safeAssetPath(const std::string& webRoot, const std::string& requestPath)
{
    std::string relative = requestPath == "/" ? "index.html" : requestPath.substr(1);
    if (relative.empty() || relative.find("..") != std::string::npos || relative.find('\\') != std::string::npos) return {};
    const std::string full = webRoot + "/" + relative;
    return regularFileExists(full) ? full : std::string();
}

std::string readFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

json mergedConfig(AdminConfigStore& store, const json& interfaces, std::string& warning)
{
    json config = store.load(&warning);
    if (!config.is_null() && !config.empty()) return config;

    std::string control;
    std::string streaming;
    if (interfaces.is_array() && !interfaces.empty()) {
        control = interfaces[0].value("name", std::string());
        streaming = interfaces.size() > 1 ? interfaces[1].value("name", std::string()) : control;
    }
    return AdminConfigStore::makeDefault(getHostname(), control, streaming);
}

bool extractChannel(const std::string& path, const std::string& prefix, std::string& channel)
{
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    channel = path.substr(prefix.size());
    return channel == "sdi1" || channel == "sdi2" || channel == "sdi3" || channel == "sdi4";
}

bool decklinkDeviceForChannel(const json& config,
                              const std::string& channel,
                              const std::string& expectedRole,
                              int& device,
                              std::string& error)
{
    if (!config.contains("sdi_ports") || !config["sdi_ports"].is_array()) {
        error = "SDI assignment configuration is unavailable";
        return false;
    }
    for (const auto& port : config["sdi_ports"]) {
        if (!port.is_object() || port.value("id", std::string()) != channel) continue;
        if (port.value("role", std::string()) != expectedRole) {
            error = channel + " is not assigned as a " + expectedRole + " in Admin";
            return false;
        }
        if (!port.contains("decklink_device") || !port["decklink_device"].is_number_integer()) {
            error = channel + " has no DeckLink device assignment";
            return false;
        }
        device = port["decklink_device"].get<int>();
        return true;
    }
    error = channel + " is not present in the SDI assignment configuration";
    return false;
}

std::string cpuProfileForConfig(const json& config)
{
    if (!config.is_object() || !config.contains("cpu") || !config["cpu"].is_object()) {
        return "system_default";
    }
    const json& cpu = config["cpu"];
    if (!cpu.contains("profile") || !cpu["profile"].is_string()) {
        return "system_default";
    }
    const std::string profile = cpu["profile"].get<std::string>();
    return profile.empty() ? "system_default" : profile;
}

bool parseJsonBody(const Request& request, json& value, std::string& error)
{
    try {
        value = json::parse(request.body);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("invalid JSON: ") + ex.what();
        return false;
    }
}

// GUI-only history. Sampling is outside media processing threads.
// Persist a narrow numeric allowlist, never an entire process status or SRT URL.
class MonitorHistory {
public:
    explicit MonitorHistory(const std::string& configPath) {
        const auto slash = configPath.rfind('/');
        path_ = (slash == std::string::npos ? std::string(".") : configPath.substr(0, slash)) + "/monitor_history.ndjson";
        load();
    }

    void sample(SenderProcessManager& manager) {
        using clock = std::chrono::system_clock;
        const auto now = clock::now();
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (timestamp - lastSampleMs_ < 5000) return;
        lastSampleMs_ = timestamp;
        for (int i = 1; i <= 4; ++i) {
            const std::string channel = "sdi" + std::to_string(i);
            const json status = manager.status(channel);
            const bool running = status.value("running", false);
            const std::string role = running ? status.value("role", std::string()) : "idle";
            const std::string session = role + ":" + std::to_string(status.value("pid", -1));
            const json rx = status.value("receiver_state", json::object());
            const json stats = rx.value("stats", json::object());
            const json srt = rx.value("srt_telemetry", json::object());
            const json playout = rx.value("decklink_telemetry", json::object());
            const json tx = status.value("sender_telemetry", json::object());
            const json perf = status.value("sender_performance", json::object());
            json item = {{"t", timestamp}, {"session", session}, {"channel", channel}};
            // Null means unavailable, never an invented zero.
            for (const auto* name : {"bitrate", "rtt", "drops", "queue", "decoderFps", "avDelta", "scheduledAvDelta", "outputDrops", "outputQueue", "buffer", "loss", "captureFps", "encodeMs", "cpu", "captureDrops"}) item[name] = nullptr;
            if (running && role == "sender" && tx.value("stale", true) == false) {
                metric(item, "bitrate", tx, "bitrate_mbps");
                metric(item, "rtt", tx, "rtt_ms");
                metric(item, "drops", tx, "packets_dropped");
                metric(item, "queue", tx, "sndbuf_ms");
                metric(item, "captureFps", perf, "capture_fps");
                metric(item, "cpu", perf, "process_cpu_percent_one_core_100");
                metric(item, "captureDrops", perf, "capture_evictions");
                metric(item, "encodeMs", perf.value("video_encode_zc", json::object()), "average_ms");
            } else if (running && role == "receiver") {
                if (srt.value("valid", false)) {
                    metric(item, "bitrate", srt, "receive_mbps");
                    metric(item, "rtt", srt, "rtt_ms");
                    metric(item, "buffer", srt, "receive_buffer_ms");
                    metric(item, "loss", srt, "packets_lost");
                }
                metric(item, "drops", stats, "decoder_video_drops");
                metric(item, "queue", stats, "decoder_video_queue");
                metric(item, "avDelta", rx, "av_queue_delta_ms");
                metric(item, "scheduledAvDelta", playout, "scheduled_av_delta_ms");
                metric(item, "outputDrops", playout, "video_drops");
                metric(item, "outputQueue", playout, "buffered_video_frames");
                const auto frames = numeric(stats, "decoded_video_frames_total");
                const auto stamp = numeric(stats, "sample_time_ms");
                const auto generation = numeric(stats, "source_generation");
                auto& prev = previous_[channel];
                if (frames && stamp && generation && prev.session == session &&
                    prev.generation == *generation && *stamp > prev.stamp &&
                    *stamp - prev.stamp <= 10000 && *frames >= prev.frames) {
                    item["decoderFps"] = (*frames - prev.frames) * 1000.0 / (*stamp - prev.stamp);
                }
                if (frames && stamp && generation && (!prev.stamp || *stamp > prev.stamp || *frames < prev.frames))
                    prev = {session, *generation, *stamp, *frames};
            }
            entries_[channel].push_back(item);
            std::ofstream out(path_, std::ios::app);
            if (out) out << item.dump() << '\n';
        }
        trim(timestamp);
        if (timestamp - lastCompactMs_ >= 3600000) { compact(); lastCompactMs_ = timestamp; }
    }

    json history(const std::string& channel) const {
        auto found = entries_.find(channel);
        if (found == entries_.end()) return json::array();
        json result = json::array();
        for (const auto& sample : found->second) result.push_back(sample);
        return result;
    }
private:
    struct Prev { std::string session; double generation=0, stamp=0, frames=0; };
    static const json* field(const json& object, const char* key) {
        if (!object.is_object()) return nullptr;
        const auto it = object.find(key);
        return it == object.end() ? nullptr : &(*it);
    }
    static std::unique_ptr<double> numeric(const json& object, const char* key) {
        const json* value = field(object, key);
        if (!value || !value->is_number()) return {};
        const double number = value->get<double>();
        if (!std::isfinite(number)) return {};
        return std::unique_ptr<double>(new double(number));
    }
    static void metric(json& item, const char* dest, const json& object, const char* source) {
        auto v = numeric(object, source);
        if (v) item[dest] = *v;
    }
    void trim(long long now) {
        for (auto& pair : entries_) {
            auto& samples = pair.second;
            while (!samples.empty() && samples.front().value("t", 0LL) < now - 86400000LL) samples.pop_front();
            while (samples.size() > 17280) samples.pop_front(); // 24h at 5s
        }
    }
    void load() {
        std::ifstream input(path_);
        if (!input) return;
        std::string line;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        while (std::getline(input, line)) {
            if (line.size() > 4096) continue;
            try {
                json item = json::parse(line);
                if (!item.is_object() || !item.contains("t") || !item["t"].is_number_integer()) continue;
                const auto t = item["t"].get<long long>();
                if (t < now - 86400000LL || t > now + 60000) continue;
                const std::string channel = item.value("channel", std::string());
                if (channel == "sdi1" || channel == "sdi2" || channel == "sdi3" || channel == "sdi4") entries_[channel].push_back(item);
            } catch (...) { /* Skip truncated final line. */ }
        }
        trim(now);
    }
    void compact() {
        const std::string temporary = path_ + ".tmp";
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return;
        for (const auto& pair : entries_)
            for (const auto& item : pair.second) out << item.dump() << '\n';
        out.close();
        if (out && ::rename(temporary.c_str(), path_.c_str()) != 0)
            std::cerr << "[GUI] Could not rotate monitoring history: " << std::strerror(errno) << "\n";
    }
    std::string path_;
    std::map<std::string, std::deque<json>> entries_;
    std::map<std::string, Prev> previous_;
    long long lastSampleMs_ = 0, lastCompactMs_ = 0;
};

void handleClient(int fd,
                  const WebServerOptions& options,
                  AdminConfigStore& configStore,
                  SenderConfigStore& senderStore,
                  ReceiverConfigStore& receiverStore,
                  SenderProcessManager& processManager,
                  MonitorHistory& monitorHistory,
                  const std::chrono::steady_clock::time_point startedAt)
{
    timeval timeout{};
    timeout.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    Request request;
    if (!readRequest(fd, request)) {
        sendJson(fd, 400, "Bad Request", {{"ok", false}, {"error", "invalid HTTP request"}});
        ::close(fd);
        return;
    }

    std::string channel;
    if (request.method == "GET" && request.path == "/api/health") {
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        sendJson(fd, 200, "OK", {{"ok", true}, {"service", "NxFrameGUI"}, {"uptime_seconds", uptime}});
    } else if (request.method == "GET" && extractChannel(request.path, "/api/monitoring/history/", channel)) {
        sendJson(fd, 200, "OK", {{"ok", true}, {"channel", channel}, {"sample_interval_ms", 5000}, {"samples", monitorHistory.history(channel)}});
    } else if (request.method == "GET" && extractChannel(request.path, "/api/monitoring/", channel)) {
        sendJson(fd, 200, "OK", processManager.status(channel));
    } else if (request.method == "GET" && extractChannel(request.path, "/api/logs/", channel)) {
        const json status = processManager.status(channel);
        const std::string worker = status.value("worker_log_path", std::string());
        const std::string completed = status.value("last_log_path", std::string());
        const std::string active = !worker.empty() && regularFileExists(worker) ? worker : completed;
        sendJson(fd, 200, "OK", {{"ok", true}, {"channel", channel},
            {"running", status.value("running", false)}, {"role", status.value("role", std::string())},
            {"source", active}, {"content", logTail(active)},
            {"source_kind", active.empty() ? "none" : (active == worker ? "live_worker" : "completed_session")}});
    } else if (request.method == "GET" && request.path == "/api/bootstrap") {
        const json interfaces = enumerateNetworkInterfaces();
        std::string warning;
        const json config = mergedConfig(configStore, interfaces, warning);
        sendJson(fd, 200, "OK", {
            {"ok", true},
            {"config", config},
            {"interfaces", interfaces},
            {"warning", warning},
            {"cpu_profiles", processManager.cpuProfiles()},
            {"cpu_profile_warning", processManager.cpuProfileWarning()},
            {"capabilities", {
                {"network_apply", false},
                {"service_control", processManager.available()},
                {"sender_configuration", true},
                {"receiver_configuration", true},
                {"cpu_profiles", true},
                {"authentication", false}
            }}
        });
    } else if (request.method == "PUT" && request.path == "/api/config") {
        json config;
        std::string error;
        if (!parseJsonBody(request, config, error)) {
            sendJson(fd, 400, "Bad Request", {{"ok", false}, {"error", error}});
        } else if (!processManager.hasCpuProfile(cpuProfileForConfig(config))) {
            sendJson(fd, 422, "Unprocessable Entity", {
                {"ok", false},
                {"error", "selected CPU profile is not available"}
            });
        } else if (processManager.anyRunning()) {
            std::string currentWarning;
            const json currentConfig = configStore.load(&currentWarning);
            if (cpuProfileForConfig(currentConfig) != cpuProfileForConfig(config)) {
                sendJson(fd, 409, "Conflict", {
                    {"ok", false},
                    {"error", "CPU profile cannot change while an SDI channel is running"}
                });
            } else if (!configStore.save(config, &error)) {
                sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
            } else {
                sendJson(fd, 200, "OK", {{"ok", true}, {"message", "configuration saved"}});
            }
        } else if (!configStore.save(config, &error)) {
            sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
        } else {
            sendJson(fd, 200, "OK", {{"ok", true}, {"message", "configuration saved"}});
        }
    } else if (request.method == "GET" && request.path == "/api/sender/templates") {
        std::string warning;
        const json templates = senderStore.listTemplates(&warning);
        sendJson(fd, 200, "OK", {{"ok", true}, {"templates", templates}, {"warning", warning}});
    } else if (request.method == "GET" && extractChannel(request.path, "/api/sender/channels/", channel)) {
        std::string warning;
        json result = senderStore.loadChannel(channel, &warning);
        result["ok"] = true;
        result["channel"] = channel;
        result["warning"] = warning;
        sendJson(fd, 200, "OK", result);
    } else if (request.method == "GET" && extractChannel(request.path, "/api/sender/status/", channel)) {
        sendJson(fd, 200, "OK", processManager.status(channel));
    } else if (request.method == "POST" && extractChannel(request.path, "/api/sender/start/", channel)) {
        std::string warning;
        const json config = configStore.load(&warning);
        int decklinkDevice = -1;
        std::string error;
        if (!decklinkDeviceForChannel(config, channel, "sender", decklinkDevice, error)) {
            sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
        } else {
            nlohmann::json response;
            const std::string presetPath = senderStore.channelPath(channel);
            if (!processManager.start(
                    channel,
                    decklinkDevice,
                    presetPath,
                    cpuProfileForConfig(config),
                    response,
                    &error)) {
                sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
            } else {
                sendJson(fd, 200, "OK", response);
            }
        }
    } else if (request.method == "POST" && extractChannel(request.path, "/api/sender/stop/", channel)) {
        std::string error;
        nlohmann::json response;
        if (!processManager.stop(channel, response, &error)) {
            sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
        } else {
            sendJson(fd, 200, "OK", response);
        }
    } else if (request.method == "POST" && extractChannel(request.path, "/api/sender/validate/", channel)) {
        json body;
        std::string error;
        if (!parseJsonBody(request, body, error)) {
            sendJson(fd, 400, "Bad Request", {{"ok", false}, {"error", error}});
        } else {
            json response;
            if (!senderStore.validateChannelRequest(channel, body, response, &error)) {
                sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
            } else {
                sendJson(fd, 200, "OK", response);
            }
        }
    } else if (request.method == "PUT" && extractChannel(request.path, "/api/sender/channels/", channel)) {
        json body;
        std::string error;
        if (!parseJsonBody(request, body, error)) {
            sendJson(fd, 400, "Bad Request", {{"ok", false}, {"error", error}});
        } else {
            json response;
            if (!senderStore.saveChannel(channel, body, response, &error)) {
                sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
            } else {
                sendJson(fd, 200, "OK", response);
            }
        }
    } else if (request.method == "GET" && extractChannel(request.path, "/api/receiver/channels/", channel)) {
        std::string warning;
        json result = receiverStore.loadChannel(channel, &warning);
        result["ok"] = true;
        result["channel"] = channel;
        result["warning"] = warning;
        sendJson(fd, 200, "OK", result);
    } else if (request.method == "GET" && extractChannel(request.path, "/api/receiver/status/", channel)) {
        sendJson(fd, 200, "OK", processManager.status(channel));
    } else if (request.method == "POST" && extractChannel(request.path, "/api/receiver/start/", channel)) {
        std::string warning;
        const json config = configStore.load(&warning);
        int decklinkDevice = -1;
        std::string error;
        if (!decklinkDeviceForChannel(config, channel, "receiver", decklinkDevice, error)) {
            sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
        } else {
            json response;
            if (!processManager.startReceiver(channel,
                                              decklinkDevice,
                                              receiverStore.channelPath(channel),
                                              cpuProfileForConfig(config),
                                              response,
                                              &error)) {
                sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
            } else {
                sendJson(fd, 200, "OK", response);
            }
        }
    } else if (request.method == "POST" && extractChannel(request.path, "/api/receiver/stop/", channel)) {
        std::string error;
        json response;
        if (!processManager.stop(channel, response, &error)) {
            sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
        } else {
            sendJson(fd, 200, "OK", response);
        }
    } else if (request.method == "POST" && extractChannel(request.path, "/api/receiver/route/", channel)) {
        json body;
        std::string error;
        if (!parseJsonBody(request, body, error)) {
            sendJson(fd, 400, "Bad Request", {{"ok", false}, {"error", error}});
        } else if (!body.contains("audio_pair_route") || !body["audio_pair_route"].is_array()) {
            sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", "audio_pair_route must be an array"}});
        } else {
            std::vector<int> route;
            bool valid = true;
            for (const auto& item : body["audio_pair_route"]) {
                if (!item.is_number_integer() || item.get<int>() < 0 || item.get<int>() > 64) {
                    valid = false;
                    break;
                }
                route.push_back(item.get<int>());
            }
            if (!valid) {
                sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", "audio route values must be integers from 0 to 64"}});
            } else {
                json response;
                if (!processManager.updateReceiverRoute(channel, route, response, &error)) {
                    sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
                } else {
                    sendJson(fd, 200, "OK", response);
                }
            }
        }
    } else if (request.method == "POST" && extractChannel(request.path, "/api/receiver/validate/", channel)) {
        json body;
        std::string error;
        if (!parseJsonBody(request, body, error)) {
            sendJson(fd, 400, "Bad Request", {{"ok", false}, {"error", error}});
        } else {
            json response;
            if (!receiverStore.validateChannelRequest(channel, body, response, &error)) {
                sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
            } else {
                sendJson(fd, 200, "OK", response);
            }
        }
    } else if (request.method == "PUT" && extractChannel(request.path, "/api/receiver/channels/", channel)) {
        json body;
        std::string error;
        if (!parseJsonBody(request, body, error)) {
            sendJson(fd, 400, "Bad Request", {{"ok", false}, {"error", error}});
        } else {
            json response;
            if (!receiverStore.saveChannel(channel, body, response, &error)) {
                sendJson(fd, 422, "Unprocessable Entity", {{"ok", false}, {"error", error}});
            } else {
                sendJson(fd, 200, "OK", response);
            }
        }
    } else if (request.path.compare(0, 5, "/api/") == 0) {
        sendJson(fd, 404, "Not Found", {{"ok", false}, {"error", "API endpoint not found"}});
    } else if (request.method == "GET") {
        std::string path = safeAssetPath(options.webRoot, request.path);
        if (path.empty() && request.path.find('.') == std::string::npos) path = safeAssetPath(options.webRoot, "/");
        if (path.empty()) {
            sendResponse(fd, 404, "Not Found", "text/plain; charset=utf-8", "Not found\n");
        } else {
            sendResponse(fd, 200, "OK", mimeType(path), readFile(path));
        }
    } else {
        sendResponse(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "Method not allowed\n",
                     "Allow: GET, PUT, POST\r\n");
    }

    ::close(fd);
}

} // namespace

int runWebServer(const WebServerOptions& options, std::atomic<bool>& shutdownRequested)
{
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        std::cerr << "[GUI] socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    (void)::fcntl(server, F_SETFD, FD_CLOEXEC);

    int reuse = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.bindAddress.c_str(), &address.sin_addr) != 1) {
        std::cerr << "[GUI] Invalid IPv4 bind address: " << options.bindAddress << "\n";
        ::close(server);
        return 1;
    }

    if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "[GUI] bind() failed on " << options.bindAddress << ':' << options.port
                  << ": " << std::strerror(errno) << "\n";
        ::close(server);
        return 1;
    }
    if (::listen(server, 32) != 0) {
        std::cerr << "[GUI] listen() failed: " << std::strerror(errno) << "\n";
        ::close(server);
        return 1;
    }

    AdminConfigStore configStore(options.configPath);
    SenderConfigStore senderStore(options.encoderPresetRoot, options.channelConfigRoot);
    ReceiverConfigStore receiverStore(options.channelConfigRoot);
    SenderProcessManager processManager(options.nxframeExecutable, options.cpuProfileConfig);
    MonitorHistory monitorHistory(options.configPath);
    const auto startedAt = std::chrono::steady_clock::now();

    std::cout << "[GUI] NxFrame dashboard: http://" << options.bindAddress << ':' << options.port << "\n"
              << "[GUI] Static files: " << options.webRoot << "\n"
              << "[GUI] Appliance configuration: " << options.configPath << "\n"
              << "[GUI] Encoder templates: " << options.encoderPresetRoot << "\n"
              << "[GUI] SDI channel presets: " << options.channelConfigRoot << "\n";
    if (processManager.available()) {
        std::cout << "[GUI] NxFrame executable: " << processManager.executable() << "\n";
    }

    while (!shutdownRequested.load(std::memory_order_acquire)) {
        processManager.reap();
        monitorHistory.sample(processManager);
        pollfd descriptor{};
        descriptor.fd = server;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 250);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[GUI] poll() failed: " << std::strerror(errno) << "\n";
            break;
        }
        if (ready == 0 || !(descriptor.revents & POLLIN)) continue;

        sockaddr_in clientAddress{};
        socklen_t clientLength = sizeof(clientAddress);
        const int client = ::accept(server, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
        if (client < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[GUI] accept() failed: " << std::strerror(errno) << "\n";
            continue;
        }
        (void)::fcntl(client, F_SETFD, FD_CLOEXEC);
        handleClient(client, options, configStore, senderStore, receiverStore, processManager, monitorHistory, startedAt);
    }

    ::close(server);
    processManager.shutdownAll();
    std::cout << "[GUI] Dashboard stopped.\n";
    return 0;
}
