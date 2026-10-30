#include "sifa/server.hpp"
#include "sifa/settings.hpp"
#include "sifa/timing.hpp"
#include "sifa/vm.hpp"
#include "third_party/json.hpp"
#include "assets.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
static void close_socket(Socket fd) { closesocket(fd); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
using Socket = int;
constexpr Socket invalid_socket = -1;
static void close_socket(Socket fd) { close(fd); }
#endif

namespace sifa {
using json = nlohmann::json;
namespace {
struct SocketOwner {
    Socket fd = invalid_socket;
    explicit SocketOwner(Socket value) : fd(value) {}
    ~SocketOwner() { if (fd != invalid_socket) close_socket(fd); }
    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;
};

json host_json(const HostInfo& h) {
    return {{"os", h.os}, {"arch",h.arch}, {"logical_cpus",h.logical_cpus}, {"memory_mb",h.memory_mb}};
}
json hypervisor_json(const HypervisorInfo& h) {
    return {{"backend",h.backend},{"available",h.available},{"detail",h.detail}};
}
json result_json(const VmResult& r) {
    return {{"ok",r.ok},{"backend",r.backend},{"output",r.output},{"error",r.error},
        {"instructions",r.instructions},{"elapsed_ms",r.elapsed_ms}};
}
json settings_json(const Settings& s) {
    return {{"preset",s.preset},{"width",s.width},{"height",s.height},
        {"target_fps",s.target_fps},{"cpu_cores",s.cpu_cores},{"memory_mb",s.memory_mb},
        {"dpi",s.dpi},{"sensitivity",s.sensitivity},{"cm360",s.cm360},{"raw_input",s.raw_input}};
}

json parse_json(const std::string& source) {
    // API/config documents are shallow; reject nesting bombs before allocating
    // an unbounded recursive object graph even within the byte-size limit.
    return json::parse(source, [](int depth, json::parse_event_t, json&) {
        if (depth > 16) throw std::invalid_argument("JSON nesting limit exceeded");
        return true;
    });
}

Settings parse_settings(const json& j, Settings s, const HostInfo& h) {
    if (!j.is_object()) throw std::invalid_argument("Settings must be a JSON object");
    const auto base = settings_json(s);
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!base.contains(it.key())) throw std::invalid_argument("Unknown setting: " + it.key());
        if (it.value().is_null()) throw std::invalid_argument("Setting cannot be null: " + it.key());
    }
    if (j.contains("preset")) {
        s.preset = j.at("preset").get<std::string>();
        if (s.preset != "balanced" && s.preset != "performance" && s.preset != "quality" && s.preset != "custom")
            throw std::invalid_argument("Unknown preset");
        if (j.size() == 1 && s.preset != "custom") {
            auto preset = settings_for_preset(s.preset, h);
            preset.dpi = s.dpi;
            preset.cm360 = s.cm360;
            preset.sensitivity = s.sensitivity;
            preset.raw_input = s.raw_input;
            return preset;
        }
    }
    auto integer = [&](const char* key, int& target) {
        if (!j.contains(key)) return;
        if (!j[key].is_number_integer()) throw std::invalid_argument(std::string(key)+" must be an integer");
        const auto value = j[key].get<double>();
        if (value < -100000000 || value > 100000000) throw std::invalid_argument("Setting is out of range");
        target = j[key].get<int>();
    };
    integer("width",s.width); integer("height",s.height); integer("target_fps",s.target_fps);
    integer("cpu_cores",s.cpu_cores); integer("memory_mb",s.memory_mb); integer("dpi",s.dpi);
    if (j.contains("sensitivity")) s.sensitivity = j["sensitivity"].get<double>();
    if (j.contains("cm360")) s.cm360 = j["cm360"].get<double>();
    if (j.contains("raw_input")) s.raw_input = j["raw_input"].get<bool>();
    return normalize_settings(s, h);
}

std::string random_token() {
    std::array<unsigned char, 32> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Cannot generate a local session key");
#else
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto n = getrandom(bytes.data()+offset, bytes.size()-offset, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("Cannot generate a local session key");
        offset += static_cast<std::size_t>(n);
    }
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    for (auto b : bytes) { token += hex[b >> 4]; token += hex[b & 15]; }
    return token;
}

std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t")-first+1);
}

struct Request { std::string method, path, body; std::map<std::string,std::string> headers; };
Request read_request(Socket socket) {
    constexpr std::size_t limit = 16384;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    auto check_deadline = [&] {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::invalid_argument("Request deadline exceeded");
    };
    std::string raw;
    std::array<char, 4096> buffer{};
    std::size_t split;
    while ((split = raw.find("\r\n\r\n")) == std::string::npos) {
        const int n = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        check_deadline();
        if (n <= 0) throw std::invalid_argument("Incomplete request");
        raw.append(buffer.data(), static_cast<std::size_t>(n));
        if (raw.size() > limit) throw std::invalid_argument("Headers too large");
    }
    Request request;
    std::istringstream stream(raw.substr(0, split));
    std::string line, version, extra;
    std::getline(stream, line);
    std::istringstream first(line);
    if (!(first >> request.method >> request.path >> version) || (first >> extra) || version != "HTTP/1.1")
        throw std::invalid_argument("Expected HTTP/1.1 request");
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) throw std::invalid_argument("Invalid header");
        const auto name = lower(line.substr(0,colon));
        if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
            throw std::invalid_argument("Invalid header name");
        if (!request.headers.emplace(name,trim(line.substr(colon+1))).second)
            throw std::invalid_argument("Duplicate header");
    }
    if (request.headers.contains("transfer-encoding")) throw std::invalid_argument("Chunked requests unsupported");
    std::size_t size = 0;
    if (request.headers.contains("content-length")) {
        const auto& text = request.headers.at("content-length");
        const auto [end, ec] = std::from_chars(text.data(), text.data()+text.size(), size);
        if (ec != std::errc{} || end != text.data()+text.size() || size > limit)
            throw std::invalid_argument("Invalid body size");
    }
    request.body = raw.substr(split+4);
    while (request.body.size() < size) {
        const int n = recv(socket, buffer.data(), static_cast<int>(std::min(buffer.size(), size-request.body.size())), 0);
        check_deadline();
        if (n <= 0) throw std::invalid_argument("Incomplete body");
        request.body.append(buffer.data(), static_cast<std::size_t>(n));
    }
    if (request.body.size() != size) throw std::invalid_argument("Unexpected trailing data");
    return request;
}

void respond(Socket socket, int status, std::string_view type, std::string_view body, std::string_view extra = {}) {
    std::ostringstream head;
    const char* reason = status == 200 ? "OK" : status == 303 ? "See Other" : status == 400 ? "Bad Request" :
        status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Internal Server Error";
    head << "HTTP/1.1 " << status << ' ' << reason << "\r\nContent-Type: " << type
         << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\nCache-Control: no-store\r\n"
         << "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
         << "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
         << "img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n"
         << extra << "\r\n";
    const std::string payload = head.str()+std::string(body);
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const int n = send(socket, payload.data()+offset,
            static_cast<int>(std::min<std::size_t>(payload.size()-offset, 65536)), 0);
        if (n <= 0) return;
        offset += static_cast<std::size_t>(n);
    }
}
void json_response(Socket s, int status, const json& j) { respond(s,status,"application/json; charset=utf-8",j.dump()); }

bool authenticated(const Request& r, const std::string& name, const std::string& token) {
    const auto it = r.headers.find("cookie");
    if (it == r.headers.end()) return false;
    std::istringstream cookies(it->second);
    std::string cookie;
    while (std::getline(cookies,cookie,';')) if (trim(cookie) == name+"="+token) return true;
    return false;
}

void save_settings(const std::filesystem::path& dir, const Settings& s) {
    std::filesystem::create_directories(dir);
    const auto path = dir/"settings.json";
    const auto temp = dir/("settings."+random_token()+".tmp");
    try {
        std::ofstream file(temp, std::ios::binary);
        file << settings_json(s).dump(2) << '\n';
        file.close();
        if (!file) throw std::runtime_error("Cannot write settings");
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace settings file");
#else
        std::filesystem::rename(temp,path);
#endif
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp,ec);
        throw;
    }
}

void open_browser(const std::string& url) {
#ifdef _WIN32
    const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteA(nullptr,"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
    if (result <= 32) std::cerr << "Could not open your browser automatically. Open the address above.\n";
#else
    const auto pid = fork();
    if (pid == 0) { execlp("xdg-open","xdg-open",url.c_str(),static_cast<char*>(nullptr)); _exit(127); }
    if (pid < 0) std::cerr << "Could not open your browser automatically. Open the address above.\n";
#endif
}
}

std::filesystem::path default_data_dir() {
#ifdef _WIN32
    if (const char* value = std::getenv("LOCALAPPDATA")) return std::filesystem::path(value)/"Sifa";
#else
    if (const char* value = std::getenv("XDG_CONFIG_HOME"); value && *value) return std::filesystem::path(value)/"sifa";
    if (const char* value = std::getenv("HOME")) return std::filesystem::path(value)/".config"/"sifa";
#endif
    return std::filesystem::current_path()/"sifa-data";
}

std::string diagnostics_json() {
    return json{{"version","0.1.0"},{"host",host_json(detect_host())},
        {"hypervisor",hypervisor_json(probe_hypervisor())},
        {"capabilities",{{"software_guest",true},{"android",false},{"game",false}}},
        {"limitations",json::array({"No Android guest, virtual GPU or ARM translation yet",
            "Hardware demo requires Windows Hypervisor Platform or accessible /dev/kvm",
            "Guest profiles and browser mouse lab do not configure a game"})}}.dump(2);
}

int serve(const ServerOptions& options) {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2),&wsa) != 0) throw std::runtime_error("Winsock initialization failed");
    struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
#else
    signal(SIGPIPE,SIG_IGN);
    signal(SIGCHLD,SIG_IGN);
#endif
    const HostInfo host = detect_host();
    const HypervisorInfo hypervisor = probe_hypervisor();
    Settings settings = default_settings(host);
    std::string config_warning;
    const auto settings_path = options.data_dir/"settings.json";
    if (std::filesystem::exists(settings_path)) {
        try {
            if (std::filesystem::file_size(settings_path) > 16384) throw std::runtime_error("Settings file too large");
            std::ifstream file(settings_path);
            const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            settings = parse_settings(parse_json(source),settings,host);
        } catch (const std::exception&) {
            config_warning = "Saved settings could not be loaded; defaults are active. Original file preserved until you save.";
            std::cerr << config_warning << '\n';
        }
    }
    json last_run = nullptr;
    SocketOwner listener(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
    if (listener.fd == invalid_socket) throw std::runtime_error("Could not create loopback socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(options.port);
    if (bind(listener.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) != 0)
        throw std::runtime_error("Could not bind loopback port; choose --port 0 for an available port");
    if (listen(listener.fd,8) != 0) throw std::runtime_error("Could not listen on loopback port");
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(listener.fd,reinterpret_cast<sockaddr*>(&address),&length) != 0)
        throw std::runtime_error("Could not determine listening port");
    const std::string authority = "127.0.0.1:"+std::to_string(ntohs(address.sin_port));
    const std::string cookie_name = "sifa_session_"+std::to_string(ntohs(address.sin_port));
    const std::string origin = "http://"+authority;
    const std::string token = random_token();
    const std::string url = origin+"/?token="+token;
    std::cout << "Sifa 0.1.0 — VM engine preview (Android not available)\n"
        << "SIFA_URL=" << url << '\n'
        << "Settings: " << settings_path.string() << "\nPress Ctrl+C to stop.\n" << std::flush;
    if (options.open_browser) open_browser(url);

    auto state = [&] { return json{{"version","0.1.0"},{"host",host_json(host)},
        {"hypervisor",hypervisor_json(hypervisor)}, {"settings",settings_json(settings)},
        {"last_run",last_run},{"config_warning",config_warning},
        {"capabilities",{{"android",false},{"game",false},{"software_guest",true}}}}; };

    for (;;) {
        SocketOwner client(accept(listener.fd,nullptr,nullptr));
        if (client.fd == invalid_socket) continue;
#ifdef _WIN32
        DWORD timeout = 3000;
        setsockopt(client.fd,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        setsockopt(client.fd,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
#else
        timeval timeout{3,0};
        setsockopt(client.fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(client.fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
#endif
        try {
            const Request req = read_request(client.fd);
            if (!req.headers.contains("host") || req.headers.at("host") != authority) {
                json_response(client.fd,403,{{"error","Invalid host"}}); continue;
            }
            if (req.headers.contains("origin") && req.headers.at("origin") != origin) {
                json_response(client.fd,403,{{"error","Cross-origin requests are not allowed"}}); continue;
            }
            if (req.method != "GET" && req.method != "POST") {
                json_response(client.fd,405,{{"error","Method not allowed"}}); continue;
            }
            if (req.method == "GET" && req.path == "/?token="+token) {
                respond(client.fd,303,"text/plain","","Location: /\r\nSet-Cookie: "+cookie_name+"="+token+"; HttpOnly; SameSite=Strict; Path=/\r\n");
                continue;
            }
            if (!authenticated(req,cookie_name,token)) {
                json_response(client.fd,403,{{"error","Open the SIFA_URL printed by the running app to start a local session"}}); continue;
            }
            if (req.method == "GET") {
                if (req.path == "/") respond(client.fd,200,"text/html; charset=utf-8",assets::html);
                else if (req.path == "/styles.css") respond(client.fd,200,"text/css; charset=utf-8",assets::css);
                else if (req.path == "/app.js") respond(client.fd,200,"text/javascript; charset=utf-8",assets::js);
                else if (req.path == "/api/state") json_response(client.fd,200,state());
                else if (req.path == "/api/diagnostics") {
                    auto d = json::parse(diagnostics_json());
                    d["settings"] = settings_json(settings); d["last_run"] = last_run; d["config_warning"] = config_warning;
                    json_response(client.fd,200,d);
                } else json_response(client.fd,404,{{"error","Not found"}});
                continue;
            }
            if (!req.headers.contains("content-type") || req.headers.at("content-type").rfind("application/json",0) != 0)
                throw std::invalid_argument("Expected application/json");
            const auto body = parse_json(req.body);
            if (!body.is_object()) throw std::invalid_argument("Expected a JSON object");
            if (req.path == "/api/settings") {
                const auto next = parse_settings(body,settings,host);
                save_settings(options.data_dir,next);
                settings = next; config_warning.clear();
                json_response(client.fd,200,settings_json(settings));
            } else if (req.path == "/api/demo") {
                const auto backend = body.value("backend",std::string("auto"));
                if (backend != "auto" && backend != "software" && backend != "hardware")
                    throw std::invalid_argument("Unknown backend");
                last_run = result_json(run_guest_demo(backend));
                json_response(client.fd,200,last_run);
            } else if (req.path == "/api/benchmark") {
                constexpr int samples = 200;
                for (int i = 0; i < 10; ++i) if (!run_guest_demo("software").ok)
                    throw std::runtime_error("Guest warmup failed");
                std::vector<double> values;
                values.reserve(samples);
                for (int i = 0; i < samples; ++i) {
                    const auto start = std::chrono::steady_clock::now();
                    const auto result = run_guest_demo("software");
                    if (!result.ok) throw std::runtime_error("Guest benchmark failed");
                    values.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
                }
                const auto stats = summarize_timings(values);
                json_response(client.fd,200,{{"samples",stats.samples},{"mean_ms",stats.mean_ms},
                    {"p50_ms",stats.p50_ms},{"p95_ms",stats.p95_ms},{"max_ms",stats.max_ms},
                    {"scope","Software guest boot + execution; not game FPS or graphics performance"}});
            } else json_response(client.fd,404,{{"error","Not found"}});
        } catch (const json::exception&) {
            json_response(client.fd,400,{{"error","Invalid JSON or setting type"}});
        } catch (const std::invalid_argument& e) {
            json_response(client.fd,400,{{"error",e.what()}});
        } catch (const std::exception& e) {
            std::cerr << "Request failed: " << e.what() << '\n';
            json_response(client.fd,500,{{"error","Operation failed; see Sifa console for details"}});
        }
    }
}
}
