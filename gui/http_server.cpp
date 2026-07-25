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

void handleClient(int fd,
                  const WebServerOptions& options,
                  AdminConfigStore& configStore,
                  SenderConfigStore& senderStore,
                  ReceiverConfigStore& receiverStore,
                  SenderProcessManager& processManager,
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
        handleClient(client, options, configStore, senderStore, receiverStore, processManager, startedAt);
    }

    ::close(server);
    processManager.shutdownAll();
    std::cout << "[GUI] Dashboard stopped.\n";
    return 0;
}
