#include <asio.hpp>
#include <crow.h>
#include <crow/middlewares/cors.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
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

void log_line(const std::string& level, const std::string& message) {
    std::cout << "[" << now_string() << "] [LemonTea] [" << level << "] " << message << std::endl;
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

struct PluginManifest {
    std::string name;
    std::string description;
    std::string executable;
    std::string script;
    uint16_t port;
    bool autoStart;
    fs::path baseDirectory;
};

struct PluginRuntime {
    PluginManifest manifest;
    pid_t pid = -1;
};

class PluginManager {
public:
    explicit PluginManager(asio::io_context& io) : io_(io) {}

    void load(const std::vector<fs::path>& manifestPaths) {
        for (const auto& manifestPath : manifestPaths) {
            auto raw = read_json_file(manifestPath);
            PluginManifest manifest;
            manifest.name = raw.at("name").get<std::string>();
            manifest.description = raw.value("description", manifest.name);
            manifest.executable = raw.value("executable", "python3");
            manifest.script = raw.at("script").get<std::string>();
            manifest.port = static_cast<uint16_t>(raw.value("port", 9200));
            manifest.autoStart = raw.value("auto_start", true);
            manifest.baseDirectory = manifestPath.parent_path();
            plugins_.emplace(manifest.name, PluginRuntime{manifest});
        }
    }

    void start_auto_plugins() {
        for (auto& [name, runtime] : plugins_) {
            if (runtime.manifest.autoStart) {
                start_plugin(name);
            }
        }
    }

    json list() const {
        json items = json::array();
        for (const auto& [name, runtime] : plugins_) {
            items.push_back(
                {
                    {"name", runtime.manifest.name},
                    {"description", runtime.manifest.description},
                    {"port", runtime.manifest.port},
                    {"running", runtime.pid > 0},
                }
            );
        }
        return items;
    }

    json call(const std::string& name, const std::string& action, const json& payload) {
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
        runtime->pid = pid;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        log_line("INFO", "started server plugin " + name);
    }

    void stop_plugin(const std::string& name) {
        auto* runtime = find(name);
        if (!runtime) {
            throw std::runtime_error("plugin not found: " + name);
        }
        if (runtime->pid > 0) {
            kill(runtime->pid, SIGTERM);
            waitpid(runtime->pid, nullptr, 0);
            runtime->pid = -1;
            log_line("INFO", "stopped server plugin " + name);
        }
    }

    void stop_all() {
        for (auto& [name, runtime] : plugins_) {
            if (runtime.pid > 0) {
                kill(runtime.pid, SIGTERM);
                waitpid(runtime.pid, nullptr, 0);
                runtime.pid = -1;
            }
        }
    }

private:
    PluginRuntime* find(const std::string& name) {
        auto it = plugins_.find(name);
        if (it == plugins_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    asio::io_context& io_;
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

class TcpClientSession final : public IClientSession, public std::enable_shared_from_this<TcpClientSession> {
public:
    TcpClientSession(tcp::socket socket, std::string clientId, std::string readBuffer)
        : socket_(std::move(socket)),
          clientId_(std::move(clientId)),
          readBuffer_(std::move(readBuffer)) {}

    ~TcpClientSession() override {
        close();
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
                    log_line("INFO", "unsolicited message type=" + type + " from " + clientId_);
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
};

class WebRtcClientSession final : public IClientSession, public std::enable_shared_from_this<WebRtcClientSession> {
public:
    WebRtcClientSession(tcp::socket signalSocket, std::string clientId, std::vector<std::string> stunServers)
        : signalSocket_(std::move(signalSocket)),
          clientId_(std::move(clientId)),
          stunServers_(std::move(stunServers)) {}

    ~WebRtcClientSession() override {
        close();
    }

    void start() override {
        rtc::InitLogger(rtc::LogLevel::Info);
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
                    log_line("INFO", "unsolicited WebRTC message type=" + type + " from " + clientId_);
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
                const auto type = payload.value("type", "");
                if (type == "description") {
                    peerConnection_->setRemoteDescription(rtc::Description(
                        payload.at("sdp").get<std::string>(),
                        payload.at("description_type").get<std::string>()
                    ));
                } else if (type == "candidate") {
                    peerConnection_->addRemoteCandidate(rtc::Candidate(
                        payload.at("candidate").get<std::string>(),
                        payload.value("mid", "")
                    ));
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
    std::atomic<bool> connected_{false};
    std::string lastSeen_ = now_string();
    std::mutex responseMutex_;
    std::condition_variable responseCv_;
    std::map<std::string, json> responses_;
    std::mutex openMutex_;
    std::condition_variable openCv_;
    inline static std::atomic<uint64_t> requestCounter_{0};
};

class LemonTeaServer {
public:
    explicit LemonTeaServer(Config config)
        : config_(std::move(config)),
          io_(),
          plugins_(io_) {
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

    std::string transport_name() const {
        return transport_mode_name(config_.transport.mode);
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
                auto session = std::make_shared<TcpClientSession>(std::move(socket), clientId, buffer);
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
                auto session = std::make_shared<WebRtcClientSession>(std::move(signalSocket), clientId, config_.transport.stunServers);
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
    std::thread acceptThread_;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        fs::path configPath = argc > 1 ? fs::path(argv[1]) : fs::path("config/server.example.json");
        auto config = load_config(configPath);
        LemonTeaServer server(config);
        server.start();

        // Enable CORS so browser frontends can call the API from other origins.
        crow::App<crow::CORSHandler> app;
        auto& cors = app.get_middleware<crow::CORSHandler>();
        cors.global().origin("*").methods("GET"_method, "POST"_method, "OPTIONS"_method).headers("Content-Type");

        CROW_ROUTE(app, "/health")([&server] {
            return json_response({
                {"service", "LemonTea"},
                {"status", "ok"},
                {"transport_mode", server.transport_name()},
                {"clients", server.list_clients()},
            });
        });

        CROW_ROUTE(app, "/api/clients").methods(crow::HTTPMethod::Get)([&server] {
            return json_response({
                {"transport_mode", server.transport_name()},
                {"clients", server.list_clients()},
            });
        });

        CROW_ROUTE(app, "/api/clients/<string>/shell").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId) {
                auto body = parse_body(request);
                auto response = server.request_client(clientId, {
                    {"type", "shell_exec"},
                    {"command", body.value("command", "")},
                });
                return json_response(response);
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/files").methods(crow::HTTPMethod::Get)(
            [&server](const crow::request& request, const std::string& clientId) {
                const char* path = request.url_params.get("path");
                auto response = server.request_client(clientId, {
                    {"type", "list_files"},
                    {"path", path ? std::string(path) : std::string()},
                });
                return json_response(response);
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/file").methods(crow::HTTPMethod::Get)(
            [&server](const crow::request& request, const std::string& clientId) {
                const char* path = request.url_params.get("path");
                auto response = server.request_client(clientId, {
                    {"type", "read_file"},
                    {"path", path ? std::string(path) : std::string()},
                });
                return json_response(response);
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/file/write").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId) {
                auto body = parse_body(request);
                auto response = server.request_client(clientId, {
                    {"type", "write_file"},
                    {"path", body.value("path", "")},
                    {"content_base64", body.value("content_base64", "")},
                });
                return json_response(response);
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/plugins").methods(crow::HTTPMethod::Get)(
            [&server](const crow::request&, const std::string& clientId) {
                auto response = server.request_client(clientId, {{"type", "plugin_list"}});
                return json_response(response);
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/plugins/<string>/call").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& clientId, const std::string& pluginName) {
                auto body = parse_body(request);
                auto response = server.request_client(clientId, {
                    {"type", "plugin_call"},
                    {"plugin", pluginName},
                    {"action", body.value("action", "get_status")},
                    {"payload", body.value("payload", json::object())},
                });
                return json_response(response);
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/plugins/<string>/start").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request&, const std::string& clientId, const std::string& pluginName) {
                auto response = server.request_client(clientId, {
                    {"type", "plugin_start"},
                    {"plugin", pluginName},
                });
                return json_response(response);
            }
        );

        CROW_ROUTE(app, "/api/clients/<string>/plugins/<string>/stop").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request&, const std::string& clientId, const std::string& pluginName) {
                auto response = server.request_client(clientId, {
                    {"type", "plugin_stop"},
                    {"plugin", pluginName},
                });
                return json_response(response);
            }
        );

        CROW_ROUTE(app, "/api/server/plugins").methods(crow::HTTPMethod::Get)([&server] {
            return json_response({
                {"transport_mode", server.transport_name()},
                {"plugins", server.plugins().list()},
            });
        });

        CROW_ROUTE(app, "/api/server/plugins/<string>/call").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request& request, const std::string& pluginName) {
                auto body = parse_body(request);
                auto response = server.plugins().call(
                    pluginName,
                    body.value("action", "get_status"),
                    body.value("payload", json::object())
                );
                return json_response(response);
            }
        );

        CROW_ROUTE(app, "/api/server/plugins/<string>/start").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request&, const std::string& pluginName) {
                server.plugins().start_plugin(pluginName);
                return json_response({{"ok", true}, {"plugin", pluginName}, {"started", true}});
            }
        );

        CROW_ROUTE(app, "/api/server/plugins/<string>/stop").methods(crow::HTTPMethod::Post)(
            [&server](const crow::request&, const std::string& pluginName) {
                server.plugins().stop_plugin(pluginName);
                return json_response({{"ok", true}, {"plugin", pluginName}, {"stopped", true}});
            }
        );

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