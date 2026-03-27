#include <asio.hpp>
#include <crow.h>
#include <crow/middlewares/cors.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using asio::ip::tcp;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string now_string() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&time, &local_tm);
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

int log_severity(const std::string& level) {
    if (level == "DEBUG") {
        return 0;
    }
    if (level == "INFO") {
        return 1;
    }
    return 2;
}

void log_line(const std::string& level, const std::string& message) {
    if (log_severity(level) < 1) {
        return;
    }
    std::cout << "[" << now_string() << "] [LemonTea] [" << level << "] " << message << std::endl;
}

constexpr const char* kLemonTeaUpdateLogPath = "/tmp/lemontea-update.log";
constexpr const char* kLemonTeaUpdateLauncherLogPath = "/tmp/lemontea-update-launcher.log";

std::string read_text_tail(const fs::path& path, std::streamoff maxBytes = 8192) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::string();
    }

    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0) {
        return std::string();
    }

    const auto start = std::max<std::streamoff>(0, size - maxBytes);
    input.seekg(start, std::ios::beg);

    std::string output(static_cast<size_t>(size - start), '\0');
    input.read(output.data(), static_cast<std::streamsize>(output.size()));
    output.resize(static_cast<size_t>(input.gcount()));
    return output;
}

std::atomic<bool> g_cleanupStarted{false};
std::function<void()> g_processCleanup;

void run_registered_cleanup() noexcept {
    if (g_cleanupStarted.exchange(true)) {
        return;
    }
    try {
        if (g_processCleanup) {
            g_processCleanup();
        }
    } catch (...) {
    }
}

void register_process_cleanup(std::function<void()> cleanup) {
    g_processCleanup = std::move(cleanup);
}

void process_signal_handler(int signum) {
    run_registered_cleanup();
    std::_Exit(128 + signum);
}

void install_process_handlers() {
    std::atexit(run_registered_cleanup);
    std::set_terminate([] {
        run_registered_cleanup();
        std::abort();
    });
    std::signal(SIGINT, process_signal_handler);
    std::signal(SIGTERM, process_signal_handler);
    std::signal(SIGABRT, process_signal_handler);
}

json read_json_file(const fs::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open json file: " + path.string());
    }
    json value;
    input >> value;
    return value;
}

std::vector<unsigned char> base64_decode(const std::string& input) {
    static const std::array<int, 256> table = [] {
        std::array<int, 256> lookup{};
        lookup.fill(-1);
        const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (size_t index = 0; index < chars.size(); ++index) {
            lookup[static_cast<unsigned char>(chars[index])] = static_cast<int>(index);
        }
        return lookup;
    }();

    std::vector<unsigned char> output;
    int value = 0;
    int valb = -8;
    for (unsigned char c : input) {
        if (table[c] == -1) {
            if (c == '=') {
                break;
            }
            continue;
        }
        value = (value << 6) + table[c];
        valb += 6;
        if (valb >= 0) {
            output.push_back(static_cast<unsigned char>((value >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return output;
}

bool is_valid_plugin_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (unsigned char ch : name) {
        if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')) {
            return false;
        }
    }
    return true;
}

fs::path validated_relative_path(const std::string& rawPath) {
    fs::path path(rawPath);
    if (rawPath.empty() || path.is_absolute()) {
        throw std::runtime_error("plugin file path must be a non-empty relative path");
    }
    for (const auto& part : path) {
        if (part == "..") {
            throw std::runtime_error("plugin file path must not escape install directory: " + rawPath);
        }
    }
    return path.lexically_normal();
}

std::string peer_state_name(rtc::PeerConnection::State state) {
    switch (state) {
        case rtc::PeerConnection::State::New:
            return "new";
        case rtc::PeerConnection::State::Connecting:
            return "connecting";
        case rtc::PeerConnection::State::Connected:
            return "connected";
        case rtc::PeerConnection::State::Disconnected:
            return "disconnected";
        case rtc::PeerConnection::State::Failed:
            return "failed";
        case rtc::PeerConnection::State::Closed:
            return "closed";
    }
    return "unknown";
}

void write_json_line(tcp::socket& socket, const json& payload) {
    auto line = payload.dump() + "\n";
    asio::write(socket, asio::buffer(line));
}

json read_json_line(tcp::socket& socket, std::string& buffer) {
    asio::read_until(socket, asio::dynamic_buffer(buffer), '\n');
    auto newline = buffer.find('\n');
    auto line = buffer.substr(0, newline);
    buffer.erase(0, newline + 1);
    if (line.empty()) {
        return json::object();
    }
    return json::parse(line);
}

enum class TransportMode {
    Tcp,
    WebRtc,
};

TransportMode parse_transport_mode(const std::string& rawMode) {
    if (rawMode == "tcp") {
        return TransportMode::Tcp;
    }
    if (rawMode == "webrtc" || rawMode == "webRTC" || rawMode == "web_rtc") {
        return TransportMode::WebRtc;
    }
    throw std::runtime_error("unsupported transport mode: " + rawMode);
}

std::string transport_mode_name(TransportMode mode) {
    return mode == TransportMode::Tcp ? "tcp" : "webrtc";
}

struct TransportConfig {
    TransportMode mode = TransportMode::Tcp;
    uint16_t tcpListenPort = 9000;
    uint16_t webrtcSignalPort = 9001;
    std::vector<std::string> stunServers;
};

struct Config {
    TransportConfig transport;
    std::string httpHost;
    uint16_t httpPort;
    int requestTimeoutMs;
    std::vector<fs::path> pluginManifestPaths;
    fs::path pluginInstallRoot;
    fs::path executablePath;
    fs::path configPath;
};

Config load_config(const fs::path& path) {
    auto raw = read_json_file(path);
    auto root = path.parent_path();
    Config config;
    if (raw.contains("transport")) {
        const auto& transport = raw.at("transport");
        config.transport.mode = parse_transport_mode(transport.value("mode", "tcp"));
        config.transport.tcpListenPort = static_cast<uint16_t>(transport.value("tcp_listen_port", raw.value("tcp_listen_port", 9000)));
        config.transport.webrtcSignalPort = static_cast<uint16_t>(transport.value("webrtc_signal_port", 9001));
        config.transport.stunServers = transport.value("stun_servers", std::vector<std::string>{});
    } else {
        config.transport.mode = TransportMode::Tcp;
        config.transport.tcpListenPort = static_cast<uint16_t>(raw.value("tcp_listen_port", 9000));
    }
    config.httpHost = raw.value("http_host", "0.0.0.0");
    config.httpPort = static_cast<uint16_t>(raw.value("http_port", 18080));
    config.requestTimeoutMs = raw.value("request_timeout_ms", 15000);
    config.pluginInstallRoot = fs::weakly_canonical(root / raw.value("plugin_install_root", std::string{"../plugins/installed"}));
    for (const auto& item : raw.value("plugin_manifests", std::vector<std::string>{})) {
        config.pluginManifestPaths.push_back(fs::weakly_canonical(root / item));
    }
    return config;
}

json parse_body(const crow::request& request) {
    if (request.body.empty()) {
        return json::object();
    }
    return json::parse(request.body);
}

std::string url_decode(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (size_t index = 0; index < input.size(); ++index) {
        const char ch = input[index];
        if (ch == '%' && index + 2 < input.size()) {
            const auto hex = std::string(input.substr(index + 1, 2));
            char* end = nullptr;
            const auto value = std::strtol(hex.c_str(), &end, 16);
            if (end != nullptr && *end == '\0') {
                output.push_back(static_cast<char>(value));
                index += 2;
                continue;
            }
        }
        if (ch == '+') {
            output.push_back(' ');
        } else {
            output.push_back(ch);
        }
    }
    return output;
}

bool starts_with_path(const fs::path& root, const fs::path& target) {
    const auto rootString = root.lexically_normal().generic_string();
    const auto targetString = target.lexically_normal().generic_string();
    return targetString == rootString || targetString.rfind(rootString + "/", 0) == 0;
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += "'";
    return quoted;
}

crow::response json_response(const json& payload, int code = 200) {
    crow::response response(code);
    response.set_header("Content-Type", "application/json; charset=utf-8");
    // Allow browser frontends to call this API during development and from other origins.
    response.set_header("Access-Control-Allow-Origin", "*");
    response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response.set_header("Access-Control-Allow-Headers", "Content-Type");
    response.body = payload.dump(2);
    return response;
}

int classify_error_status(const std::string& message) {
    if (message.find("not found") != std::string::npos) {
        return 404;
    }
    if (message.find("timeout") != std::string::npos) {
        return 504;
    }
    if (message.find("disconnected") != std::string::npos ||
        message.find("unavailable") != std::string::npos ||
        message.find("failed") != std::string::npos) {
        return 502;
    }
    return 500;
}

template <typename Handler>
crow::response guarded_json_response(Handler&& handler, int successCode = 200) {
    try {
        return json_response(handler(), successCode);
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        log_line("ERROR", message);
        return json_response({
            {"ok", false},
            {"error", message},
        }, classify_error_status(message));
    }
}

struct PluginManifest {
    std::string name;
    std::string description;
    std::string version;
    std::string executable;
    std::string script;
    uint16_t port;
    bool autoStart;
    int protocolVersion;
    std::vector<std::string> capabilities;
    fs::path baseDirectory;
    fs::path manifestPath;
};

struct PluginRuntime {
    PluginManifest manifest;
    pid_t pid = -1;
};

class PluginManager {
public:
    PluginManager(asio::io_context& io, fs::path installRoot)
        : io_(io), installRoot_(std::move(installRoot)) {}

    void load(const std::vector<fs::path>& manifestPaths) {
        for (const auto& manifestPath : manifestPaths) {
            load_manifest_file(manifestPath, false);
        }
    }

    void start_auto_plugins() {
        for (auto& [name, runtime] : plugins_) {
            if (runtime.manifest.autoStart) {
                start_plugin(name);
            }
        }
    }

    json list() {
        refresh_process_states();
        json items = json::array();
        for (const auto& [name, runtime] : plugins_) {
            items.push_back(runtime_summary(runtime));
        }
        return items;
    }

    json call(const std::string& name, const std::string& action, const json& payload) {
        refresh_process_states();
        auto* runtime = find(name);
        if (!runtime) {
            throw std::runtime_error("plugin not found: " + name);
        }
        tcp::resolver resolver(io_);
        tcp::socket socket(io_);
        asio::connect(socket, resolver.resolve("127.0.0.1", std::to_string(runtime->manifest.port)));
        write_json_line(socket, {{"action", action}, {"payload", payload}});
        std::string buffer;
        return read_json_line(socket, buffer);
    }

    void start_plugin(const std::string& name) {
        refresh_process_states();
        auto* runtime = find(name);
        if (!runtime) {
            throw std::runtime_error("plugin not found: " + name);
        }
        if (runtime->pid > 0) {
            return;
        }
        auto scriptPath = runtime->manifest.baseDirectory / runtime->manifest.script;
        pid_t pid = fork();
        if (pid < 0) {
            throw std::runtime_error("fork failed for plugin: " + name);
        }
        if (pid == 0) {
            setpgid(0, 0);
            chdir(runtime->manifest.baseDirectory.c_str());
            std::string port = std::to_string(runtime->manifest.port);
            execlp(
                runtime->manifest.executable.c_str(),
                runtime->manifest.executable.c_str(),
                scriptPath.filename().c_str(),
                port.c_str(),
                static_cast<char*>(nullptr)
            );
            _exit(127);
        }
        setpgid(pid, pid);
        runtime->pid = pid;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        log_line("INFO", "started server plugin " + name);
    }

    void stop_plugin(const std::string& name) {
        refresh_process_states();
        auto* runtime = find(name);
        if (!runtime) {
            throw std::runtime_error("plugin not found: " + name);
        }
        if (runtime->pid > 0) {
            terminate_process_group(runtime->pid);
            runtime->pid = -1;
            log_line("INFO", "stopped server plugin " + name);
        }
    }

    void stop_all() {
        refresh_process_states();
        for (auto& [name, runtime] : plugins_) {
            if (runtime.pid > 0) {
                terminate_process_group(runtime.pid);
                runtime.pid = -1;
            }
        }
    }

    void emergency_stop_all() noexcept {
        for (auto& [name, runtime] : plugins_) {
            if (runtime.pid > 0) {
                kill_process_group(runtime.pid, SIGTERM);
                runtime.pid = -1;
            }
        }
    }

    json install(const json& manifestRaw, const json& filesPayload, bool replaceExisting) {
        refresh_process_states();

        const auto name = manifestRaw.value("name", std::string());
        if (!is_valid_plugin_name(name)) {
            throw std::runtime_error("plugin manifest must provide a safe name using letters, numbers, '.', '_' or '-'");
        }

        fs::create_directories(installRoot_);
        auto installDirectory = installRoot_ / name;
        if (fs::exists(installDirectory)) {
            if (!replaceExisting) {
                throw std::runtime_error("plugin already exists: " + name);
            }
            stop_plugin(name);
            plugins_.erase(name);
            fs::remove_all(installDirectory);
        }

        fs::create_directories(installDirectory);
        for (const auto& file : filesPayload) {
            auto relativePath = validated_relative_path(file.at("path").get<std::string>());
            auto targetPath = installDirectory / relativePath;
            fs::create_directories(targetPath.parent_path());
            auto content = base64_decode(file.at("content_base64").get<std::string>());
            std::ofstream output(targetPath, std::ios::binary);
            output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        }

        auto manifestPath = installDirectory / "plugin.manifest.json";
        {
            std::ofstream output(manifestPath);
            output << manifestRaw.dump(2);
        }

        auto& runtime = load_manifest_file(manifestPath, true);
        auto entryScript = runtime.manifest.baseDirectory / runtime.manifest.script;
        if (!fs::exists(entryScript)) {
            plugins_.erase(runtime.manifest.name);
            throw std::runtime_error("plugin entry script not found after installation: " + entryScript.string());
        }
        if (runtime.manifest.autoStart) {
            start_plugin(runtime.manifest.name);
        }
        return runtime_summary(runtime);
    }

private:
    static void kill_process_group(pid_t pid, int signum) noexcept {
        if (pid <= 0) {
            return;
        }
        if (killpg(pid, signum) != 0 && errno == ESRCH) {
            kill(pid, signum);
        }
    }

    static void terminate_process_group(pid_t pid) {
        if (pid <= 0) {
            return;
        }
        kill_process_group(pid, SIGTERM);
        for (int attempt = 0; attempt < 20; ++attempt) {
            int status = 0;
            auto result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                return;
            }
            if (result == -1 && errno == ECHILD) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        kill_process_group(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }

    PluginManifest parse_manifest(const json& raw, const fs::path& manifestPath) const {
        PluginManifest manifest;
        manifest.name = raw.at("name").get<std::string>();
        if (!is_valid_plugin_name(manifest.name)) {
            throw std::runtime_error("invalid plugin name: " + manifest.name);
        }
        manifest.description = raw.value("description", manifest.name);
        manifest.version = raw.value("version", std::string{"1.0.0"});
        manifest.executable = raw.value("executable", std::string{"python3"});
        manifest.script = validated_relative_path(raw.at("script").get<std::string>()).generic_string();
        manifest.port = static_cast<uint16_t>(raw.value("port", 9200));
        if (manifest.port == 0) {
            throw std::runtime_error("plugin port must be greater than 0");
        }
        manifest.autoStart = raw.value("auto_start", true);
        manifest.protocolVersion = raw.value("protocol_version", 1);
        manifest.capabilities = raw.value("capabilities", std::vector<std::string>{});
        manifest.baseDirectory = manifestPath.parent_path();
        manifest.manifestPath = manifestPath;
        return manifest;
    }

    PluginRuntime& load_manifest_file(const fs::path& manifestPath, bool replaceExisting) {
        PluginRuntime runtime{parse_manifest(read_json_file(manifestPath), manifestPath)};
        auto it = plugins_.find(runtime.manifest.name);
        if (it != plugins_.end()) {
            if (!replaceExisting) {
                throw std::runtime_error("duplicate plugin name: " + runtime.manifest.name);
            }
            it->second = std::move(runtime);
            return it->second;
        }
        auto [inserted, _] = plugins_.emplace(runtime.manifest.name, std::move(runtime));
        return inserted->second;
    }

    json runtime_summary(const PluginRuntime& runtime) const {
        return {
            {"name", runtime.manifest.name},
            {"description", runtime.manifest.description},
            {"version", runtime.manifest.version},
            {"port", runtime.manifest.port},
            {"running", runtime.pid > 0},
            {"auto_start", runtime.manifest.autoStart},
            {"protocol_version", runtime.manifest.protocolVersion},
            {"capabilities", runtime.manifest.capabilities},
            {"manifest_path", runtime.manifest.manifestPath.string()},
        };
    }

    void refresh_process_states() {
        for (auto& [name, runtime] : plugins_) {
            if (runtime.pid <= 0) {
                continue;
            }
            int status = 0;
            auto result = waitpid(runtime.pid, &status, WNOHANG);
            if (result == runtime.pid || (result == -1 && errno == ECHILD)) {
                runtime.pid = -1;
                log_line("INFO", "server plugin exited: " + name);
            }
        }
    }

    PluginRuntime* find(const std::string& name) {
        auto it = plugins_.find(name);
        if (it == plugins_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    asio::io_context& io_;
    fs::path installRoot_;
    std::map<std::string, PluginRuntime> plugins_;
};

class IClientSession {
public:
    virtual ~IClientSession() = default;
    virtual void start() = 0;
    virtual void join() = 0;
    virtual void close() = 0;
    virtual json send_request(json request, int timeoutMs) = 0;
    virtual json snapshot() const = 0;
    virtual std::string client_id() const = 0;
};

using ClientEventCallback = std::function<void(const std::string&, const json&)>;

class TcpClientSession final : public IClientSession, public std::enable_shared_from_this<TcpClientSession> {
public:
    TcpClientSession(tcp::socket socket, std::string clientId, std::string readBuffer, ClientEventCallback eventCallback)
        : socket_(std::move(socket)),
          clientId_(std::move(clientId)),
          readBuffer_(std::move(readBuffer)),
          eventCallback_(std::move(eventCallback)) {}

    ~TcpClientSession() override {
        close();
        join();
    }

    void start() override {
        reader_ = std::thread([self = shared_from_this()] { self->read_loop(); });
    }

    void join() override {
        if (reader_.joinable()) {
            reader_.join();
        }
    }

    void close() override {
        bool expected = true;
        if (connected_.compare_exchange_strong(expected, false)) {
            std::error_code ignored;
            socket_.close(ignored);
        }
    }

    json send_request(json request, int timeoutMs) override {
        if (!connected_) {
            throw std::runtime_error("client disconnected: " + clientId_);
        }
        const std::string requestId = std::to_string(++requestCounter_);
        request["request_id"] = requestId;

        {
            std::lock_guard<std::mutex> lock(responseMutex_);
            responses_.erase(requestId);
        }
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            write_json_line(socket_, request);
        }

        std::unique_lock<std::mutex> lock(responseMutex_);
        bool ready = responseCv_.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [&] { return responses_.count(requestId) > 0 || !connected_; }
        );
        if (!ready || responses_.count(requestId) == 0) {
            throw std::runtime_error("request timeout for client: " + clientId_);
        }
        auto payload = responses_.at(requestId);
        responses_.erase(requestId);
        return payload;
    }

    json snapshot() const override {
        return {
            {"client_id", clientId_},
            {"connected", connected_.load()},
            {"last_seen", lastSeen_},
            {"transport", "tcp"},
        };
    }

    std::string client_id() const override {
        return clientId_;
    }

private:
    void read_loop() {
        try {
            while (connected_) {
                auto message = read_json_line(socket_, readBuffer_);
                if (message.is_null() || message.empty()) {
                    continue;
                }
                const auto type = message.value("type", "");
                if (type == "response") {
                    auto requestId = message.value("request_id", "");
                    {
                        std::lock_guard<std::mutex> lock(responseMutex_);
                        responses_[requestId] = message;
                    }
                    responseCv_.notify_all();
                } else if (type == "heartbeat") {
                    lastSeen_ = message.value("timestamp", now_string());
                    log_line("DEBUG", "heartbeat from " + clientId_);
                } else {
                    if (eventCallback_) {
                        eventCallback_(clientId_, message);
                    }
                    log_line("DEBUG", "unsolicited message type=" + type + " from " + clientId_);
                }
            }
        } catch (const std::exception& ex) {
            log_line("ERROR", "client " + clientId_ + " disconnected: " + ex.what());
        }
        connected_ = false;
        responseCv_.notify_all();
        std::error_code ignored;
        socket_.close(ignored);
    }

    tcp::socket socket_;
    std::string clientId_;
    std::string readBuffer_;
    std::atomic<bool> connected_{true};
    std::string lastSeen_ = now_string();
    std::thread reader_;
    std::mutex writeMutex_;
    std::mutex responseMutex_;
    std::condition_variable responseCv_;
    std::map<std::string, json> responses_;
    inline static std::atomic<uint64_t> requestCounter_{0};
    ClientEventCallback eventCallback_;
};

class WebRtcClientSession final : public IClientSession, public std::enable_shared_from_this<WebRtcClientSession> {
public:
    WebRtcClientSession(tcp::socket signalSocket, std::string clientId, std::vector<std::string> stunServers, ClientEventCallback eventCallback)
        : signalSocket_(std::move(signalSocket)),
          clientId_(std::move(clientId)),
          stunServers_(std::move(stunServers)),
          eventCallback_(std::move(eventCallback)) {}

    ~WebRtcClientSession() override {
        close();
        join();
    }

    void start() override {
        rtc::InitLogger(rtc::LogLevel::Warning);
        rtc::Configuration rtcConfig;
        for (const auto& stunServer : stunServers_) {
            rtcConfig.iceServers.emplace_back(stunServer);
        }
        peerConnection_ = std::make_shared<rtc::PeerConnection>(rtcConfig);
        configure_peer();
        auto localChannel = peerConnection_->createDataChannel("control");
        attach_data_channel(localChannel);
        signalThread_ = std::thread([self = shared_from_this()] { self->signal_loop(); });
        wait_until_open();
    }

    void join() override {
        if (signalThread_.joinable()) {
            signalThread_.join();
        }
    }

    void close() override {
        bool wasConnected = connected_.exchange(false);
        if (!wasConnected && !signalSocket_.is_open()) {
            return;
        }
        responseCv_.notify_all();
        openCv_.notify_all();
        std::error_code ignored;
        signalSocket_.close(ignored);
        auto dc = data_channel();
        if (dc) {
            dc->close();
        }
        if (peerConnection_) {
            peerConnection_->close();
        }
    }

    json send_request(json request, int timeoutMs) override {
        if (!connected_) {
            throw std::runtime_error("client disconnected: " + clientId_);
        }
        const std::string requestId = std::to_string(++requestCounter_);
        request["request_id"] = requestId;

        {
            std::lock_guard<std::mutex> lock(responseMutex_);
            responses_.erase(requestId);
        }

        auto dc = data_channel();
        if (!dc) {
            throw std::runtime_error("WebRTC data channel unavailable for client: " + clientId_);
        }
        dc->send(request.dump());

        std::unique_lock<std::mutex> lock(responseMutex_);
        bool ready = responseCv_.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [&] { return responses_.count(requestId) > 0 || !connected_; }
        );
        if (!ready || responses_.count(requestId) == 0) {
            throw std::runtime_error("request timeout for client: " + clientId_);
        }
        auto payload = responses_.at(requestId);
        responses_.erase(requestId);
        return payload;
    }

    json snapshot() const override {
        return {
            {"client_id", clientId_},
            {"connected", connected_.load()},
            {"last_seen", lastSeen_},
            {"transport", "webrtc"},
        };
    }

    std::string client_id() const override {
        return clientId_;
    }

private:
    void configure_peer() {
        peerConnection_->onStateChange([this](rtc::PeerConnection::State state) {
            log_line("INFO", "WebRTC state for " + clientId_ + " -> " + peer_state_name(state));
            if (state == rtc::PeerConnection::State::Disconnected ||
                state == rtc::PeerConnection::State::Failed ||
                state == rtc::PeerConnection::State::Closed) {
                connected_ = false;
                responseCv_.notify_all();
                openCv_.notify_all();
            }
        });

        peerConnection_->onLocalDescription([this](rtc::Description description) {
            send_signal({
                {"type", "description"},
                {"description_type", description.typeString()},
                {"sdp", std::string(description)},
            });
        });

        peerConnection_->onLocalCandidate([this](rtc::Candidate candidate) {
            send_signal({
                {"type", "candidate"},
                {"candidate", candidate.candidate()},
                {"mid", candidate.mid()},
            });
        });
    }

    void attach_data_channel(std::shared_ptr<rtc::DataChannel> channel) {
        {
            std::lock_guard<std::mutex> lock(dataChannelMutex_);
            dataChannel_ = std::move(channel);
        }
        auto dc = data_channel();
        dc->onOpen([this] {
            connected_ = true;
            openCv_.notify_all();
            log_line("INFO", "WebRTC data channel opened for " + clientId_);
        });
        dc->onClosed([this] {
            connected_ = false;
            responseCv_.notify_all();
            openCv_.notify_all();
        });
        dc->onMessage([this](std::variant<rtc::binary, rtc::string> message) {
            try {
                std::string text;
                if (std::holds_alternative<rtc::string>(message)) {
                    text = std::get<rtc::string>(message);
                } else {
                    const auto& binary = std::get<rtc::binary>(message);
                    text.reserve(binary.size());
                    for (const auto& value : binary) {
                        text.push_back(static_cast<char>(value));
                    }
                }
                auto payload = json::parse(text);
                const auto type = payload.value("type", "");
                if (type == "response") {
                    std::lock_guard<std::mutex> lock(responseMutex_);
                    responses_[payload.value("request_id", "")] = payload;
                    responseCv_.notify_all();
                } else if (type == "heartbeat") {
                    lastSeen_ = payload.value("timestamp", now_string());
                } else if (type == "hello") {
                    lastSeen_ = now_string();
                } else {
                    if (eventCallback_) {
                        eventCallback_(clientId_, payload);
                    }
                    log_line("DEBUG", "unsolicited WebRTC message type=" + type + " from " + clientId_);
                }
            } catch (const std::exception& ex) {
                log_line("ERROR", std::string("failed to parse WebRTC payload for ") + clientId_ + ": " + ex.what());
            }
        });
    }

    std::shared_ptr<rtc::DataChannel> data_channel() const {
        std::lock_guard<std::mutex> lock(dataChannelMutex_);
        return dataChannel_;
    }

    void send_signal(const json& payload) {
        std::lock_guard<std::mutex> lock(signalWriteMutex_);
        if (!signalSocket_.is_open()) {
            return;
        }
        write_json_line(signalSocket_, payload);
    }

    void wait_until_open() {
        std::unique_lock<std::mutex> lock(openMutex_);
        bool ready = openCv_.wait_for(lock, std::chrono::seconds(20), [&] {
            return data_channel() != nullptr && connected_.load();
        });
        if (!ready) {
            throw std::runtime_error("timeout waiting for WebRTC client channel open: " + clientId_);
        }
    }

    void signal_loop() {
        try {
            while (signalSocket_.is_open()) {
                auto payload = read_json_line(signalSocket_, signalBuffer_);
                if (payload.is_null() || payload.empty()) {
                    continue;
                }
                try {
                    const auto type = payload.value("type", "");
                    if (type == "description") {
                        peerConnection_->setRemoteDescription(rtc::Description(
                            payload.at("sdp").get<std::string>(),
                            payload.at("description_type").get<std::string>()
                        ));

                        std::vector<std::pair<std::string, std::string>> pendingCandidates;
                        {
                            std::lock_guard<std::mutex> lock(signalStateMutex_);
                            remoteDescriptionSet_ = true;
                            pendingCandidates.swap(pendingRemoteCandidates_);
                        }
                        for (const auto& candidate : pendingCandidates) {
                            peerConnection_->addRemoteCandidate(rtc::Candidate(candidate.first, candidate.second));
                        }
                    } else if (type == "candidate") {
                        const auto candidate = payload.at("candidate").get<std::string>();
                        const auto mid = payload.value("mid", std::string());
                        bool shouldQueue = false;
                        {
                            std::lock_guard<std::mutex> lock(signalStateMutex_);
                            shouldQueue = !remoteDescriptionSet_;
                            if (shouldQueue) {
                                pendingRemoteCandidates_.emplace_back(candidate, mid);
                            }
                        }
                        if (!shouldQueue) {
                            peerConnection_->addRemoteCandidate(rtc::Candidate(candidate, mid));
                        }
                    }
                } catch (const std::exception& ex) {
                    log_line("ERROR", "WebRTC signaling payload failed for " + clientId_ + ": " + ex.what());
                }
            }
        } catch (const std::exception& ex) {
            if (connected_) {
                log_line("ERROR", "WebRTC signaling failed for " + clientId_ + ": " + ex.what());
            }
        }
        connected_ = false;
        responseCv_.notify_all();
        openCv_.notify_all();
    }

    tcp::socket signalSocket_;
    std::string signalBuffer_;
    std::string clientId_;
    std::vector<std::string> stunServers_;
    std::thread signalThread_;
    std::shared_ptr<rtc::PeerConnection> peerConnection_;
    mutable std::mutex dataChannelMutex_;
    std::shared_ptr<rtc::DataChannel> dataChannel_;
    std::mutex signalWriteMutex_;
    std::mutex signalStateMutex_;
    bool remoteDescriptionSet_ = false;
    std::vector<std::pair<std::string, std::string>> pendingRemoteCandidates_;
    std::atomic<bool> connected_{false};
    std::string lastSeen_ = now_string();
    std::mutex responseMutex_;
    std::condition_variable responseCv_;
    std::map<std::string, json> responses_;
    std::mutex openMutex_;
    std::condition_variable openCv_;
    inline static std::atomic<uint64_t> requestCounter_{0};
    ClientEventCallback eventCallback_;
};

class LemonTeaServer {
public:
    using EventHandler = std::function<void(const std::string&, const json&)>;

    explicit LemonTeaServer(Config config)
        : config_(std::move(config)),
          io_(),
                    plugins_(io_, config_.pluginInstallRoot) {
        if (config_.transport.mode == TransportMode::Tcp) {
            tcpAcceptor_ = std::make_unique<tcp::acceptor>(io_, tcp::endpoint(tcp::v4(), config_.transport.tcpListenPort));
        } else {
            signalAcceptor_ = std::make_unique<tcp::acceptor>(io_, tcp::endpoint(tcp::v4(), config_.transport.webrtcSignalPort));
        }
    }

    ~LemonTeaServer() {
        stop();
    }

    void start() {
        plugins_.load(config_.pluginManifestPaths);
        plugins_.start_auto_plugins();
        acceptThread_ = std::thread([this] {
            if (config_.transport.mode == TransportMode::Tcp) {
                accept_loop_tcp();
            } else {
                accept_loop_webrtc();
            }
        });
    }

    void stop() {
        running_ = false;
        std::error_code ignored;
        if (tcpAcceptor_) {
            tcpAcceptor_->close(ignored);
        }
        if (signalAcceptor_) {
            signalAcceptor_->close(ignored);
        }
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }

        std::vector<std::shared_ptr<IClientSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            for (auto& [id, session] : clients_) {
                sessions.push_back(session);
            }
            clients_.clear();
        }
        for (auto& session : sessions) {
            session->close();
        }
        for (auto& session : sessions) {
            session->join();
        }
        plugins_.stop_all();
    }

    json list_clients() const {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        json items = json::array();
        for (const auto& [id, session] : clients_) {
            items.push_back(session->snapshot());
        }
        return items;
    }

    json request_client(const std::string& clientId, json request) {
        auto session = get_client(clientId);
        return session->send_request(std::move(request), config_.requestTimeoutMs);
    }

    PluginManager& plugins() {
        return plugins_;
    }

    void emergency_cleanup() noexcept {
        plugins_.emergency_stop_all();
    }

    std::string transport_name() const {
        return transport_mode_name(config_.transport.mode);
    }

    int subscribe_events(EventHandler handler) {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        const int subscriptionId = ++eventSubscriptionCounter_;
        eventHandlers_[subscriptionId] = std::move(handler);
        return subscriptionId;
    }

    void unsubscribe_events(int subscriptionId) {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        eventHandlers_.erase(subscriptionId);
    }

private:
    std::shared_ptr<IClientSession> get_client(const std::string& clientId) const {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        auto it = clients_.find(clientId);
        if (it == clients_.end()) {
            throw std::runtime_error("client not found: " + clientId);
        }
        return it->second;
    }

    void register_client(const std::shared_ptr<IClientSession>& session) {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        auto it = clients_.find(session->client_id());
        if (it != clients_.end()) {
            it->second->close();
            it->second->join();
        }
        clients_[session->client_id()] = session;
    }

    void dispatch_event(const std::string& clientId, const json& payload) {
        std::vector<EventHandler> handlers;
        {
            std::lock_guard<std::mutex> lock(eventHandlersMutex_);
            for (const auto& item : eventHandlers_) {
                handlers.push_back(item.second);
            }
        }
        for (const auto& handler : handlers) {
            try {
                handler(clientId, payload);
            } catch (const std::exception& ex) {
                log_line("ERROR", std::string("event handler failed: ") + ex.what());
            }
        }
    }

    void accept_loop_tcp() {
        while (running_) {
            try {
                tcp::socket socket(io_);
                tcpAcceptor_->accept(socket);
                std::string buffer;
                auto hello = read_json_line(socket, buffer);
                if (hello.value("type", "") != "hello") {
                    throw std::runtime_error("first message must be hello");
                }
                auto clientId = hello.value("client_id", "unknown-client");
                auto session = std::make_shared<TcpClientSession>(
                    std::move(socket),
                    clientId,
                    buffer,
                    [this](const std::string& id, const json& payload) { dispatch_event(id, payload); }
                );
                register_client(session);
                session->start();
                log_line("INFO", "registered TCP client " + clientId);
            } catch (const std::exception& ex) {
                if (running_) {
                    log_line("ERROR", ex.what());
                }
            }
        }
    }

    void accept_loop_webrtc() {
        while (running_) {
            try {
                tcp::socket signalSocket(io_);
                signalAcceptor_->accept(signalSocket);
                std::string buffer;
                auto hello = read_json_line(signalSocket, buffer);
                if (hello.value("type", "") != "signal_hello") {
                    throw std::runtime_error("first signaling message must be signal_hello");
                }
                auto clientId = hello.value("client_id", "unknown-client");
                auto session = std::make_shared<WebRtcClientSession>(
                    std::move(signalSocket),
                    clientId,
                    config_.transport.stunServers,
                    [this](const std::string& id, const json& payload) { dispatch_event(id, payload); }
                );
                session->start();
                register_client(session);
                log_line("INFO", "registered WebRTC client " + clientId);
            } catch (const std::exception& ex) {
                if (running_) {
                    log_line("ERROR", ex.what());
                }
            }
        }
    }

    Config config_;
    std::atomic<bool> running_{true};
    asio::io_context io_;
    std::unique_ptr<tcp::acceptor> tcpAcceptor_;
    std::unique_ptr<tcp::acceptor> signalAcceptor_;
    PluginManager plugins_;
    mutable std::mutex clientsMutex_;
    std::map<std::string, std::shared_ptr<IClientSession>> clients_;
    std::mutex eventHandlersMutex_;
    std::map<int, EventHandler> eventHandlers_;
    std::atomic<int> eventSubscriptionCounter_{0};
    std::thread acceptThread_;
};

class TerminalBridge {
public:
    explicit TerminalBridge(LemonTeaServer& server)
        : server_(server) {
        subscriptionId_ = server_.subscribe_events(
            [this](const std::string& clientId, const json& payload) {
                forward_client_event(clientId, payload);
            }
        );
    }

    ~TerminalBridge() {
        server_.unsubscribe_events(subscriptionId_);
    }

    void on_open(crow::websocket::connection& conn) {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        browserSessions_[&conn] = BrowserSession{};
    }

    void on_close(crow::websocket::connection& conn) {
        BrowserSession session;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            auto it = browserSessions_.find(&conn);
            if (it == browserSessions_.end()) {
                return;
            }
            session = it->second;
            browserSessions_.erase(it);
        }

        if (!session.clientId.empty() && !session.remoteSessionId.empty()) {
            try {
                server_.request_client(session.clientId, {
                    {"type", "pty_close"},
                    {"session_id", session.remoteSessionId},
                });
            } catch (const std::exception& ex) {
                log_line("ERROR", std::string("failed to close remote PTY session: ") + ex.what());
            }
        }
    }

    void on_message(crow::websocket::connection& conn, const std::string& data, bool isBinary) {
        if (isBinary) {
            send_json(conn, {{"type", "error"}, {"message", "binary websocket messages are not supported"}});
            return;
        }

        try {
            auto payload = json::parse(data);
            const auto action = payload.value("action", "");
            if (action == "open") {
                handle_open(conn, payload);
            } else if (action == "input") {
                handle_input(conn, payload);
            } else if (action == "resize") {
                handle_resize(conn, payload);
            } else if (action == "close") {
                handle_close(conn);
            } else {
                send_json(conn, {{"type", "error"}, {"message", "unsupported terminal action"}});
            }
        } catch (const std::exception& ex) {
            send_json(conn, {{"type", "error"}, {"message", ex.what()}});
        }
    }

private:
    struct BrowserSession {
        std::string clientId;
        std::string remoteSessionId;
    };

    BrowserSession require_browser_session(crow::websocket::connection& conn) {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = browserSessions_.find(&conn);
        if (it == browserSessions_.end()) {
            throw std::runtime_error("terminal websocket session not initialized");
        }
        return it->second;
    }

    void handle_open(crow::websocket::connection& conn, const json& payload) {
        const auto clientId = payload.value("client_id", std::string());
        if (clientId.empty()) {
            throw std::runtime_error("client_id is required for terminal open");
        }

        auto response = server_.request_client(clientId, {
            {"type", "pty_open"},
            {"cwd", payload.value("cwd", std::string())},
            {"rows", payload.value("rows", 24)},
            {"cols", payload.value("cols", 80)},
            {"shell", payload.value("shell", std::string())},
        });

        if (!response.value("ok", false)) {
            throw std::runtime_error(response.value("error", std::string("failed to open PTY session")));
        }

        auto data = response.value("data", json::object());
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            auto& session = browserSessions_[&conn];
            session.clientId = clientId;
            session.remoteSessionId = data.value("session_id", "");
        }
        send_json(conn, {
            {"type", "pty_opened"},
            {"client_id", clientId},
            {"session_id", data.value("session_id", "")},
            {"cwd", data.value("cwd", "")},
            {"shell", data.value("shell", "")},
        });
    }

    void handle_input(crow::websocket::connection& conn, const json& payload) {
        auto session = require_browser_session(conn);
        if (session.remoteSessionId.empty()) {
            throw std::runtime_error("terminal session has not been opened");
        }
        auto response = server_.request_client(session.clientId, {
            {"type", "pty_input"},
            {"session_id", session.remoteSessionId},
            {"data_base64", payload.value("data_base64", "")},
        });
        if (!response.value("ok", false)) {
            throw std::runtime_error(response.value("error", std::string("failed to send PTY input")));
        }
    }

    void handle_resize(crow::websocket::connection& conn, const json& payload) {
        auto session = require_browser_session(conn);
        if (session.remoteSessionId.empty()) {
            return;
        }
        auto response = server_.request_client(session.clientId, {
            {"type", "pty_resize"},
            {"session_id", session.remoteSessionId},
            {"rows", payload.value("rows", 24)},
            {"cols", payload.value("cols", 80)},
        });
        if (!response.value("ok", false)) {
            throw std::runtime_error(response.value("error", std::string("failed to resize PTY session")));
        }
    }

    void handle_close(crow::websocket::connection& conn) {
        BrowserSession session;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            auto it = browserSessions_.find(&conn);
            if (it == browserSessions_.end()) {
                return;
            }
            session = it->second;
            it->second.remoteSessionId.clear();
        }

        if (session.clientId.empty() || session.remoteSessionId.empty()) {
            return;
        }

        auto response = server_.request_client(session.clientId, {
            {"type", "pty_close"},
            {"session_id", session.remoteSessionId},
        });
        if (!response.value("ok", false)) {
            throw std::runtime_error(response.value("error", std::string("failed to close PTY session")));
        }
    }

    void forward_client_event(const std::string& clientId, const json& payload) {
        const auto type = payload.value("type", "");
        if (type != "pty_output" && type != "pty_exit") {
            return;
        }

        const auto sessionId = payload.value("session_id", std::string());
        std::vector<crow::websocket::connection*> targets;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            for (const auto& item : browserSessions_) {
                if (item.second.clientId == clientId && item.second.remoteSessionId == sessionId) {
                    targets.push_back(item.first);
                }
            }
            if (type == "pty_exit") {
                for (auto* target : targets) {
                    auto it = browserSessions_.find(target);
                    if (it != browserSessions_.end()) {
                        it->second.remoteSessionId.clear();
                    }
                }
            }
        }

        for (auto* target : targets) {
            send_json(*target, payload);
        }
    }

    void send_json(crow::websocket::connection& conn, const json& payload) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        conn.send_text(payload.dump());
    }

    LemonTeaServer& server_;
    int subscriptionId_ = 0;
    std::mutex sessionsMutex_;
    std::unordered_map<crow::websocket::connection*, BrowserSession> browserSessions_;
    std::mutex sendMutex_;
};

class RuntimeManager {
public:
    RuntimeManager(fs::path executablePath, fs::path configPath, std::function<void()> cleanup)
        : executablePath_(std::move(executablePath)),
          configPath_(std::move(configPath)),
          cleanup_(std::move(cleanup)) {}

    json stage_upload(const std::string& relativePath, const std::string& contentBase64, bool append) const {
        auto target = stage_root() / validated_relative_path(relativePath);
        fs::create_directories(target.parent_path());

        auto data = base64_decode(contentBase64);
        std::ofstream output(
            target,
            std::ios::binary | std::ios::out | (append ? std::ios::app : std::ios::trunc)
        );
        if (!output) {
            throw std::runtime_error("failed to open runtime staging file: " + target.string());
        }
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        output.flush();

        std::error_code sizeError;
        const auto finalSize = fs::file_size(target, sizeError);
        return {
            {"path", relativePath},
            {"staged_path", target.string()},
            {"written", static_cast<long long>(data.size())},
            {"append", append},
            {"size", sizeError ? -1 : static_cast<long long>(finalSize)},
        };
    }

    json schedule_update(const std::string& filename, const std::string& stagedPath, bool restartOnly) {
        if (executablePath_.empty()) {
            throw std::runtime_error("current executable path is unknown");
        }
        if (updateScheduled_.exchange(true)) {
            throw std::runtime_error("LemonTea update is already scheduled");
        }

        fs::remove(fs::path(kLemonTeaUpdateLogPath));
        fs::remove(fs::path(kLemonTeaUpdateLauncherLogPath));

        const auto restartScriptPath = executablePath_.string() + ".restart.sh";
        auto stagedBinaryPath = executablePath_.string();
        if (!restartOnly) {
            auto resolvedStage = resolve_existing_stage_path(stagedPath);
            if (!fs::exists(resolvedStage) || !fs::is_regular_file(resolvedStage)) {
                throw std::runtime_error("staged LemonTea binary not found: " + resolvedStage.string());
            }
            stagedBinaryPath = resolvedStage.string();
        }

        {
            std::ofstream script(restartScriptPath, std::ios::trunc);
            script << "#!/bin/sh\n";
            script << "set -eu\n";
            script << "SRC=$1\n";
            script << "DST=$2\n";
            script << "CFG=$3\n";
            script << "LOG='" << kLemonTeaUpdateLogPath << "'\n";
            script << "echo \"[$(date '+%Y-%m-%d %H:%M:%S')] update script started\" >> \"$LOG\"\n";
            script << "sleep 1\n";
            script << "if [ \"$SRC\" != \"$DST\" ]; then\n";
            script << "  TMP=\"${DST}.tmp\"\n";
            script << "  echo \"[$(date '+%Y-%m-%d %H:%M:%S')] copying staged binary\" >> \"$LOG\"\n";
            script << "  cp \"$SRC\" \"$TMP\"\n";
            script << "  chmod +x \"$TMP\"\n";
            script << "  mv \"$TMP\" \"$DST\"\n";
            script << "  rm -f \"$SRC\"\n";
            script << "  echo \"[$(date '+%Y-%m-%d %H:%M:%S')] binary replaced\" >> \"$LOG\"\n";
            script << "fi\n";
            script << "echo \"[$(date '+%Y-%m-%d %H:%M:%S')] launching LemonTea\" >> \"$LOG\"\n";
            script << "nohup \"$DST\" \"$CFG\" >>\"$LOG\" 2>&1 &\n";
            script << "echo \"[$(date '+%Y-%m-%d %H:%M:%S')] restart launched\" >> \"$LOG\"\n";
            script << "rm -f \"$0\"\n";
        }

        fs::permissions(
            restartScriptPath,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                fs::perms::group_read | fs::perms::group_exec |
                fs::perms::others_read | fs::perms::others_exec,
            fs::perm_options::replace
        );

        std::thread([this, stagedBinaryPath, restartScriptPath] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            {
                std::ofstream launcherLog(kLemonTeaUpdateLauncherLogPath, std::ios::app);
                launcherLog << "[" << now_string() << "] launching restart script" << std::endl;
            }

            const auto launchCommand =
                std::string("nohup /bin/sh ") +
                shell_quote(restartScriptPath) + " " +
                shell_quote(stagedBinaryPath) + " " +
                shell_quote(executablePath_.string()) + " " +
                shell_quote(configPath_.string()) +
                " >>" + shell_quote(kLemonTeaUpdateLauncherLogPath) + " 2>&1 &";
            std::system(launchCommand.c_str());
            if (cleanup_) {
                cleanup_();
            }
            std::_Exit(0);
        }).detach();

        return {
            {"scheduled", true},
            {"filename", filename},
            {"staged_path", stagedBinaryPath},
            {"target_path", executablePath_.string()},
            {"config_path", configPath_.string()},
            {"restart_only", restartOnly},
            {"message", "LemonTea update scheduled, service will restart automatically"},
        };
    }

    json status() const {
        const std::string updateLog = read_text_tail(fs::path(kLemonTeaUpdateLogPath));
        const std::string launcherLog = read_text_tail(fs::path(kLemonTeaUpdateLauncherLogPath));

        std::string stage = "idle";
        if (updateScheduled_.load()) {
            stage = "scheduled";
        }
        if (launcherLog.find("launching restart script") != std::string::npos) {
            stage = "launcher_started";
        }
        if (updateLog.find("copying staged binary") != std::string::npos) {
            stage = "replacing_binary";
        }
        if (updateLog.find("binary replaced") != std::string::npos) {
            stage = "binary_replaced";
        }
        if (updateLog.find("launching LemonTea") != std::string::npos) {
            stage = "restarting";
        }
        if (updateLog.find("restart launched") != std::string::npos) {
            stage = "restart_launched";
        }

        return {
            {"scheduled", updateScheduled_.load()},
            {"stage", stage},
            {"log_path", kLemonTeaUpdateLogPath},
            {"launcher_log_path", kLemonTeaUpdateLauncherLogPath},
            {"log_excerpt", updateLog},
            {"launcher_log_excerpt", launcherLog},
        };
    }

private:
    static fs::path stage_root() {
        return fs::temp_directory_path() / "lemontea-runtime-stage";
    }

    static fs::path resolve_existing_stage_path(const std::string& rawPath) {
        if (rawPath.empty()) {
            throw std::runtime_error("staged LemonTea binary path is required");
        }

        fs::path candidate(rawPath);
        if (candidate.is_relative()) {
            return stage_root() / validated_relative_path(rawPath);
        }

        auto normalized = fs::weakly_canonical(candidate);
        if (!starts_with_path(stage_root(), normalized)) {
            throw std::runtime_error("staged LemonTea binary must stay inside runtime staging directory");
        }
        return normalized;
    }

    fs::path executablePath_;
    fs::path configPath_;
    std::function<void()> cleanup_;
    std::atomic<bool> updateScheduled_{false};
};

}  // namespace

int main(int argc, char** argv) {
    try {
        install_process_handlers();
        fs::path configPath = argc > 1 ? fs::path(argv[1]) : fs::path("config/server.example.json");
        auto config = load_config(configPath);
        config.configPath = fs::absolute(configPath).lexically_normal();
        config.executablePath = fs::absolute(fs::path(argv[0])).lexically_normal();
        LemonTeaServer server(config);
        register_process_cleanup([&server] {
            server.emergency_cleanup();
        });
        server.start();
        auto runtimeManager = std::make_shared<RuntimeManager>(
            config.executablePath,
            config.configPath,
            [&server] { server.emergency_cleanup(); }
        );

        // Enable CORS so browser frontends can call the API from other origins.
        crow::App<crow::CORSHandler> app;
        auto& cors = app.get_middleware<crow::CORSHandler>();
        cors.global().origin("*").methods("GET"_method, "POST"_method, "OPTIONS"_method).headers("Content-Type");
        app.loglevel(crow::LogLevel::Warning);
        auto terminalBridge = std::make_shared<TerminalBridge>(server);

        CROW_ROUTE(app, "/health")([&server] {
            return guarded_json_response([&] {
                return json{
                {"service", "LemonTea"},
                {"status", "ok"},
                {"transport_mode", server.transport_name()},
                {"clients", server.list_clients()},
                };
            });
        });

        CROW_ROUTE(app, "/api/clients").methods(crow::HTTPMethod::Get)([&server] {
            return guarded_json_response([&] {
                return json{
                {"transport_mode", server.transport_name()},
                {"clients", server.list_clients()},
                };
            });
        });

        CROW_ROUTE(app, "/api/clients/<string>/shell").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    auto body = parse_body(request);
                    return server.request_client(decodedClientId, {
                        {"type", "shell_exec"},
                        {"command", body.value("command", "")},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/files").methods(crow::HTTPMethod::Get)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    const char* path = request.url_params.get("path");
                    return server.request_client(decodedClientId, {
                        {"type", "list_files"},
                        {"path", path ? std::string(path) : std::string()},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/file").methods(crow::HTTPMethod::Get)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    const char* path = request.url_params.get("path");
                    return server.request_client(decodedClientId, {
                        {"type", "read_file"},
                        {"path", path ? std::string(path) : std::string()},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/file/chunk").methods(crow::HTTPMethod::Get)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    const char* path = request.url_params.get("path");
                    const char* offsetParam = request.url_params.get("offset");
                    const char* lengthParam = request.url_params.get("length");
                    long long offset = 0;
                    int length = 4096;
                    if (offsetParam != nullptr) {
                        offset = std::max(0LL, std::atoll(offsetParam));
                    }
                    if (lengthParam != nullptr) {
                        length = std::max(1, std::atoi(lengthParam));
                    }
                    return server.request_client(decodedClientId, {
                        {"type", "read_file_chunk"},
                        {"path", path ? std::string(path) : std::string()},
                        {"offset", offset},
                        {"length", length},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/file/write").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    auto body = parse_body(request);
                    return server.request_client(decodedClientId, {
                        {"type", "write_file"},
                        {"path", body.value("path", "")},
                        {"content_base64", body.value("content_base64", "")},
                        {"append", body.value("append", false)},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/directory/create").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    auto body = parse_body(request);
                    return server.request_client(decodedClientId, {
                        {"type", "create_directory"},
                        {"path", body.value("path", "")},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/path/rename").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    auto body = parse_body(request);
                    return server.request_client(decodedClientId, {
                        {"type", "rename_path"},
                        {"old_path", body.value("old_path", "")},
                        {"new_path", body.value("new_path", "")},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/path/delete").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    auto body = parse_body(request);
                    return server.request_client(decodedClientId, {
                        {"type", "delete_path"},
                        {"path", body.value("path", "")},
                        {"recursive", body.value("recursive", true)},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/plugins").methods(crow::HTTPMethod::Get)(
            [&server](const crow::request&, const std::string& clientId) {
                return guarded_json_response([&] {
                    return server.request_client(url_decode(clientId), {{"type", "plugin_list"}});
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/plugins/install").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    auto body = parse_body(request);
                    return server.request_client(decodedClientId, {
                        {"type", "plugin_install"},
                        {"manifest", body.value("manifest", json::object())},
                        {"files", body.value("files", json::array())},
                        {"replace", body.value("replace", false)},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/plugins/<string>/call").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId, const std::string& pluginName) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    auto body = parse_body(request);
                    return server.request_client(decodedClientId, {
                        {"type", "plugin_call"},
                        {"plugin", pluginName},
                        {"action", body.value("action", "get_status")},
                        {"payload", body.value("payload", json::object())},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/plugins/<string>/start").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request&, const std::string& clientId, const std::string& pluginName) {
                return guarded_json_response([&] {
                    return server.request_client(url_decode(clientId), {
                        {"type", "plugin_start"},
                        {"plugin", pluginName},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/plugins/<string>/stop").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request&, const std::string& clientId, const std::string& pluginName) {
                return guarded_json_response([&] {
                    return server.request_client(url_decode(clientId), {
                        {"type", "plugin_stop"},
                        {"plugin", pluginName},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/firmware").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId) {
                return guarded_json_response([&] {
                    const auto decodedClientId = url_decode(clientId);
                    auto body = parse_body(request);
                    return server.request_client(decodedClientId, {
                        {"type", "firmware_update"},
                        {"filename", body.value("filename", std::string("honeytea"))},
                        {"content_base64", body.value("content_base64", "")},
                        {"staged_path", body.value("staged_path", "")},
                        {"restart_only", body.value("restart_only", false)},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/firmware/status").methods(crow::HTTPMethod::Get)(
            [&server](const crow::request&, const std::string& clientId) {
                return guarded_json_response([&] {
                    return server.request_client(url_decode(clientId), {
                        {"type", "firmware_status"},
                    });
                });
            }
        );

        CROW_ROUTE(app, "/api/server/plugins").methods(crow::HTTPMethod::Get)([&server] {
            return guarded_json_response([&] {
                return json{
                    {"transport_mode", server.transport_name()},
                    {"plugins", server.plugins().list()},
                };
            });
        });

        CROW_ROUTE(app, "/api/server/plugins/install").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request) {
                return guarded_json_response([&] {
                    auto body = parse_body(request);
                    auto response = server.plugins().install(
                        body.value("manifest", json::object()),
                        body.value("files", json::array()),
                        body.value("replace", false)
                    );
                    return json{{"ok", true}, {"plugin", response}};
                });
            }
        );

        CROW_ROUTE(app, "/api/server/plugins/<string>/call").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& pluginName) {
                return guarded_json_response([&] {
                    auto body = parse_body(request);
                    return server.plugins().call(
                        pluginName,
                        body.value("action", "get_status"),
                        body.value("payload", json::object())
                    );
                });
            }
        );

        CROW_ROUTE(app, "/api/server/plugins/<string>/start").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request&, const std::string& pluginName) {
                return guarded_json_response([&] {
                    server.plugins().start_plugin(pluginName);
                    return json{{"ok", true}, {"plugin", pluginName}, {"started", true}};
                });
            }
        );

        CROW_ROUTE(app, "/api/server/plugins/<string>/stop").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request&, const std::string& pluginName) {
                return guarded_json_response([&] {
                    server.plugins().stop_plugin(pluginName);
                    return json{{"ok", true}, {"plugin", pluginName}, {"stopped", true}};
                });
            }
        );

        CROW_ROUTE(app, "/api/server/runtime/status").methods(crow::HTTPMethod::Get)(
            [runtimeManager](const crow::request&) {
                return guarded_json_response([&] {
                    return runtimeManager->status();
                });
            }
        );

        CROW_ROUTE(app, "/api/server/runtime/file/write").methods(crow::HTTPMethod::Post)(
            [runtimeManager](const crow::request& request) {
                return guarded_json_response([&] {
                    auto body = parse_body(request);
                    return runtimeManager->stage_upload(
                        body.value("path", std::string()),
                        body.value("content_base64", std::string()),
                        body.value("append", false)
                    );
                });
            }
        );

        CROW_ROUTE(app, "/api/server/runtime").methods(crow::HTTPMethod::Post)(
            [runtimeManager](const crow::request& request) {
                return guarded_json_response([&] {
                    auto body = parse_body(request);
                    return runtimeManager->schedule_update(
                        body.value("filename", std::string("lemontea")),
                        body.value("staged_path", std::string()),
                        body.value("restart_only", false)
                    );
                });
            }
        );

        CROW_WEBSOCKET_ROUTE(app, "/ws/pty")
            .onopen([terminalBridge](crow::websocket::connection& conn) {
                terminalBridge->on_open(conn);
            })
            .onclose([terminalBridge](crow::websocket::connection& conn, const std::string&) {
                terminalBridge->on_close(conn);
            })
            .onmessage([terminalBridge](crow::websocket::connection& conn, const std::string& data, bool isBinary) {
                terminalBridge->on_message(conn, data, isBinary);
            });

        if (config.transport.mode == TransportMode::Tcp) {
            log_line("INFO", "TCP listen on 0.0.0.0:" + std::to_string(config.transport.tcpListenPort));
        } else {
            log_line("INFO", "WebRTC signaling listen on 0.0.0.0:" + std::to_string(config.transport.webrtcSignalPort));
        }
        log_line("INFO", "HTTP listen on " + config.httpHost + ":" + std::to_string(config.httpPort));
        log_line("INFO", "transport mode: " + transport_mode_name(config.transport.mode));
        app.bindaddr(config.httpHost).port(config.httpPort).multithreaded().run();

        server.stop();
        return 0;
    } catch (const std::exception& ex) {
        log_line("ERROR", ex.what());
        return 1;
    }
}