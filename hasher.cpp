// hasher.cpp – High-performance multi-algorithm hash calculator
// Version: 2.0.0
// Requires OpenSSL 3.x
// Compile: g++ -std=c++17 -O3 -pthread -o hasher hasher.cpp -lcrypto -lssl
// Usage:   hasher [options] <input> [algorithm] [<input2> ...]
//
// Config file: ~/.hasher/config (or HASHER_CONFIG env var)
// Format: key = value  (supports # comments)
// Keys: default_algorithm, buffer_size_mb, color, format, progress, threads, verbose

#ifdef __linux__
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif
#endif

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/provider.h>
#include <openssl/opensslv.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/hmac.h>  // still needed for HMAC_CTX? No, we'll use EVP_MAC

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

// ----------------------------------------------------------------------
// Version
// ----------------------------------------------------------------------
#define HASHER_VERSION "2.0.0"

#ifndef HASHER_DEFAULT_ALGO
#define HASHER_DEFAULT_ALGO "sha256"
#endif

// ----------------------------------------------------------------------
// Helper: get default algorithm from environment or compile-time default
// ----------------------------------------------------------------------
static std::string get_default_algorithm() {
    const char* env_algo = std::getenv("HASHER_DEFAULT_ALGO");
    if (env_algo && env_algo[0] != '\0') {
        std::string algo = env_algo;
        for (auto& c : algo) c = tolower(c);
        return algo;
    }
    return HASHER_DEFAULT_ALGO;
}

static void print_version() {
    std::cout << "hasher version " << HASHER_VERSION << "\n"
              << "OpenSSL: " << OPENSSL_VERSION_TEXT << "\n"
              << "Default algorithm: " << get_default_algorithm() << "\n"
              << "Requires OpenSSL 3.0 or later.\n";
}

// ----------------------------------------------------------------------
// Hex conversion
// ----------------------------------------------------------------------
static std::string to_hex(const unsigned char* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0x0F]);
    }
    return out;
}

// ----------------------------------------------------------------------
// Color support
// ----------------------------------------------------------------------
enum class ColorMode { ALWAYS, NEVER, AUTO };
static ColorMode color_mode = ColorMode::AUTO;

static bool use_color() {
    if (color_mode == ColorMode::ALWAYS) return true;
    if (color_mode == ColorMode::NEVER) return false;
    return isatty(fileno(stdout)) != 0;
}

#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_RED     "\033[31m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"

// ----------------------------------------------------------------------
// Output format
// ----------------------------------------------------------------------
enum class OutputFormat { TEXT, JSON, CSV };
static OutputFormat output_format = OutputFormat::TEXT;

static std::string escape_json(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                } else {
                    o << c;
                }
        }
    }
    return o.str();
}

static std::string escape_csv(const std::string& s) {
    if (s.find(',') != std::string::npos || s.find('"') != std::string::npos || s.find('\n') != std::string::npos) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\"\"";
            else out += c;
        }
        out += "\"";
        return out;
    }
    return s;
}

// ----------------------------------------------------------------------
// Config structure
// ----------------------------------------------------------------------
struct Config {
    bool show_progress = false;
    bool no_progress = false;
    bool verbose = false;
    bool force_file = false;
    bool force_string = false;
    size_t buffer_size = 64 * 1024 * 1024;
    int num_threads = 1;
    std::string algorithm = HASHER_DEFAULT_ALGO;
    std::vector<std::string> inputs;
    int digest_bits = 0;
    bool show_version = false;
    OutputFormat output_format = OutputFormat::TEXT;
    ColorMode color_mode = ColorMode::AUTO;
    std::string hmac_key;
    std::string check_file;
    std::string generate_file;
    bool generate_checksum = false;
};

// ----------------------------------------------------------------------
// Config file loader (simple key=value)
// ----------------------------------------------------------------------
static std::string get_home_dir() {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    return home ? std::string(home) : "";
#else
    const char* home = getenv("HOME");
    return home ? std::string(home) : "";
#endif
}

static std::string get_config_path() {
    const char* env_path = getenv("HASHER_CONFIG");
    if (env_path && env_path[0] != '\0') {
        return std::string(env_path);
    }
    std::string home = get_home_dir();
    if (home.empty()) return "";
    return home + "/.hasher/config";
}

static void load_config(const std::string& path, Config& cfg) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t");
        if (end == std::string::npos) continue;
        line = line.substr(start, end - start + 1);

        if (line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t") + 1);

        if (key == "default_algorithm") {
            cfg.algorithm = val;
            for (auto& c : cfg.algorithm) c = tolower(c);
        }
        else if (key == "buffer_size_mb") {
            try { cfg.buffer_size = std::stoull(val) * 1024 * 1024; }
            catch (...) { /* ignore */ }
        }
        else if (key == "color") {
            if (val == "always") cfg.color_mode = ColorMode::ALWAYS;
            else if (val == "never") cfg.color_mode = ColorMode::NEVER;
            else if (val == "auto") cfg.color_mode = ColorMode::AUTO;
        }
        else if (key == "format") {
            if (val == "json") cfg.output_format = OutputFormat::JSON;
            else if (val == "csv") cfg.output_format = OutputFormat::CSV;
            else if (val == "text") cfg.output_format = OutputFormat::TEXT;
        }
        else if (key == "progress") {
            if (val == "true" || val == "1" || val == "on") cfg.show_progress = true;
            else if (val == "false" || val == "0" || val == "off") cfg.show_progress = false;
        }
        else if (key == "threads") {
            try { cfg.num_threads = std::stoi(val); if (cfg.num_threads < 1) cfg.num_threads = 1; }
            catch (...) { /* ignore */ }
        }
        else if (key == "verbose") {
            if (val == "true" || val == "1" || val == "on") cfg.verbose = true;
            else if (val == "false" || val == "0" || val == "off") cfg.verbose = false;
        }
    }
}

// ----------------------------------------------------------------------
// Native I/O FileHandle (cross-platform)
// ----------------------------------------------------------------------
class FileHandle {
public:
#ifdef _WIN32
    using Handle = HANDLE;
#else
    using Handle = int;
#endif

    FileHandle() : handle_(invalid_value()) {}
    ~FileHandle() { close(); }

    bool open(const std::string& path) {
#ifdef _WIN32
        handle_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        return handle_ != INVALID_HANDLE_VALUE;
#else
        handle_ = ::open(path.c_str(), O_RDONLY | O_LARGEFILE);
        if (handle_ != -1) {
#ifdef __linux__
            posix_fadvise(handle_, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
        }
        return handle_ != -1;
#endif
    }

    ssize_t read(void* buf, size_t count) {
#ifdef _WIN32
        DWORD bytes_read = 0;
        if (!ReadFile(handle_, buf, (DWORD)count, &bytes_read, NULL))
            return -1;
        return (ssize_t)bytes_read;
#else
        return ::read(handle_, buf, count);
#endif
    }

    void close() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (handle_ != -1) {
            ::close(handle_);
            handle_ = -1;
        }
#endif
    }

    uint64_t size() {
#ifdef _WIN32
        LARGE_INTEGER li;
        if (!GetFileSizeEx(handle_, &li)) return 0;
        return li.QuadPart;
#else
        struct stat st;
        if (fstat(handle_, &st) != 0) return 0;
        return st.st_size;
#endif
    }

    Handle get() const { return handle_; }

private:
    Handle handle_;
#ifdef _WIN32
    static Handle invalid_value() { return INVALID_HANDLE_VALUE; }
#else
    static Handle invalid_value() { return -1; }
#endif
};

// ----------------------------------------------------------------------
// Parse command line arguments (overrides config)
// ----------------------------------------------------------------------
void parse_args(int argc, char* argv[], Config& cfg) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        std::cout << "Usage: hasher [OPTIONS] <input> [algorithm] [<input2> ...]\n"
                  << "  If multiple inputs are given, they are hashed in parallel (--threads).\n"
                  << "  When number of inputs exceeds --threads, they are processed in batches.\n"
                  << "  <input>   : string to hash, or file path (use --file or --string to disambiguate)\n"
                  << "  algorithm : md4, md5, md5-sha1, sha1, sha224, sha256, sha384, sha512,\n"
                  << "              sha512-224, sha512-256, sha3-224, sha3-256, sha3-384, sha3-512,\n"
                  << "              shake128, shake256, blake2b512, blake2s256,\n"
                  << "              ripemd160, sm3, whirlpool, mdc2,\n"
                  << "              keccak224, keccak256, keccak384, keccak512\n"
                  << "  Options:\n"
                  << "    --help, -h         Show this help\n"
                  << "    --version, -V      Show version and exit\n"
                  << "    --file             Treat all <input> as file paths\n"
                  << "    --string           Treat all <input> as literal strings\n"
                  << "    --progress, -p     Show progress bar (single file only, ignored for multiple)\n"
                  << "    --no-progress      Disable progress bar\n"
                  << "    --buffer-size=<MB> I/O buffer size in MB (default: 64)\n"
                  << "    --threads=<N>      Number of parallel file processes (default: 1)\n"
                  << "    --verbose, -v      Print detailed info (speed, split hashes for md5-sha1)\n"
                  << "    --digest-bits=<N>  For SHAKE, output length in bits\n"
                  << "    --format <fmt>     Output format: text (default), json, csv\n"
                  << "    --color <mode>     Color mode: auto (default), always, never\n"
                  << "  Hash verification & generation:\n"
                  << "    --check <file>     Read hashfile and verify each line (like sha256sum -c)\n"
                  << "    --generate <file>  Write standard checksum file (e.g., file.sha256)\n"
                  << "  HMAC:\n"
                  << "    --hmac <key>       Compute HMAC with the given key\n"
                  << "  Environment:\n"
                  << "    HASHER_DEFAULT_ALGO  Set default algorithm\n"
                  << "    HASHER_CONFIG       Path to config file (default: ~/.hasher/config)\n"
                  << "  Config file format: key = value (supports # comments)\n"
                  << "    Keys: default_algorithm, buffer_size_mb, color, format, progress, threads, verbose\n"
                  << "  Default algorithm: " << cfg.algorithm << "\n";
        exit(0);
    }

    std::vector<std::string> positional;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--version" || a == "-V") {
            cfg.show_version = true;
        }
        else if (a == "--file") {
            cfg.force_file = true;
            cfg.force_string = false;
        }
        else if (a == "--string") {
            cfg.force_string = true;
            cfg.force_file = false;
        }
        else if (a == "--progress" || a == "-p") {
            cfg.show_progress = true;
            cfg.no_progress = false;
        }
        else if (a == "--no-progress") {
            cfg.no_progress = true;
            cfg.show_progress = false;
        }
        else if (a == "--verbose" || a == "-v") {
            cfg.verbose = true;
        }
        else if (a.rfind("--buffer-size=", 0) == 0) {
            std::string val = a.substr(14);
            cfg.buffer_size = std::stoull(val) * 1024 * 1024;
            if (cfg.buffer_size < 1024*1024) cfg.buffer_size = 1024*1024;
        }
        else if (a.rfind("--threads=", 0) == 0) {
            std::string val = a.substr(10);
            cfg.num_threads = std::stoi(val);
            if (cfg.num_threads < 1) cfg.num_threads = 1;
        }
        else if (a.rfind("--digest-bits=", 0) == 0) {
            cfg.digest_bits = std::stoi(a.substr(14));
            if (cfg.digest_bits < 8) cfg.digest_bits = 8;
        }
        else if (a == "--format") {
            if (i + 1 < args.size()) {
                std::string fmt = args[++i];
                if (fmt == "json") cfg.output_format = OutputFormat::JSON;
                else if (fmt == "csv") cfg.output_format = OutputFormat::CSV;
                else if (fmt == "text") cfg.output_format = OutputFormat::TEXT;
                else { std::cerr << "Unknown format: " << fmt << std::endl; exit(1); }
            } else {
                std::cerr << "Missing argument for --format" << std::endl;
                exit(1);
            }
        }
        else if (a == "--color") {
            if (i + 1 < args.size()) {
                std::string cm = args[++i];
                if (cm == "always") cfg.color_mode = ColorMode::ALWAYS;
                else if (cm == "never") cfg.color_mode = ColorMode::NEVER;
                else if (cm == "auto") cfg.color_mode = ColorMode::AUTO;
                else { std::cerr << "Unknown color mode: " << cm << std::endl; exit(1); }
            } else {
                std::cerr << "Missing argument for --color" << std::endl;
                exit(1);
            }
        }
        else if (a == "--hmac") {
            if (i + 1 < args.size()) {
                cfg.hmac_key = args[++i];
            } else {
                std::cerr << "Missing argument for --hmac" << std::endl;
                exit(1);
            }
        }
        else if (a == "--check") {
            if (i + 1 < args.size()) {
                cfg.check_file = args[++i];
            } else {
                std::cerr << "Missing argument for --check" << std::endl;
                exit(1);
            }
        }
        else if (a == "--generate") {
            if (i + 1 < args.size()) {
                cfg.generate_file = args[++i];
                cfg.generate_checksum = true;
            } else {
                std::cerr << "Missing argument for --generate" << std::endl;
                exit(1);
            }
        }
        else if (a[0] == '-') {
            std::cerr << "Unknown option: " << a << std::endl;
            exit(1);
        }
        else {
            positional.push_back(a);
        }
    }

    if (cfg.check_file.empty() && cfg.generate_checksum == false && positional.empty() && !cfg.show_version) {
        std::cerr << "Error: Missing input.\n";
        exit(1);
    }

    if (!cfg.check_file.empty()) {
        // --check mode: no positional needed
    } else if (cfg.generate_checksum) {
        if (positional.empty()) {
            std::cerr << "Error: --generate requires at least one input file.\n";
            exit(1);
        }
        cfg.inputs = positional;
    } else {
        if (positional.size() >= 2) {
            cfg.algorithm = positional.back();
            for (auto& c : cfg.algorithm) c = tolower(c);
            for (size_t i = 0; i < positional.size() - 1; ++i)
                cfg.inputs.push_back(positional[i]);
        } else if (!positional.empty()) {
            cfg.inputs = {positional[0]};
        }
    }

    ::color_mode = cfg.color_mode;
    ::output_format = cfg.output_format;
}

// ----------------------------------------------------------------------
// Smart progress bar with ETA and speed
// ----------------------------------------------------------------------
class SmartProgressBar {
public:
    SmartProgressBar(uint64_t total, const std::string& label = "")
        : total_(total), label_(label), done_(0), last_pct_(-1), start_time_(std::chrono::steady_clock::now()) {}

    void update(uint64_t bytes) {
        if (total_ == 0) return;
        done_ += bytes;
        int pct = static_cast<int>(done_ * 100 / total_);
        if (pct != last_pct_) {
            last_pct_ = pct;
            print(pct);
        }
    }

    void finish() {
        if (last_pct_ != 100) print(100);
        std::cerr << std::endl;
    }

private:
    uint64_t total_;
    std::string label_;
    std::atomic<uint64_t> done_;
    std::atomic<int> last_pct_;
    std::chrono::time_point<std::chrono::steady_clock> start_time_;
    std::mutex mtx_;

    void print(int pct) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();
        double speed = (elapsed > 0) ? (done_ / (1024.0 * 1024.0) / elapsed) : 0.0;
        double eta = (speed > 0) ? (total_ - done_) / (1024.0 * 1024.0) / speed : 0.0;

        std::string color = use_color() ? COLOR_BLUE : "";
        std::string reset = use_color() ? COLOR_RESET : "";
        std::ostringstream oss;
        oss << "\r" << color << label_ << reset << " [";
        int bar_len = 40;
        int filled = pct * bar_len / 100;
        oss << std::string(filled, '=') << (pct < 100 ? ">" : "") << std::string(bar_len - filled - (pct < 100 ? 1 : 0), ' ');
        oss << "] " << pct << "%";
        oss << "  " << std::fixed << std::setprecision(1) << speed << " MB/s";
        if (pct < 100 && eta > 0) {
            oss << "  ETA: " << std::fixed << std::setprecision(0) << eta << "s";
        }
        std::cerr << oss.str() << std::flush;
    }
};

// ----------------------------------------------------------------------
// Security warnings
// ----------------------------------------------------------------------
const std::unordered_map<std::string, std::string> warnings = {
    {"md4",        "WARNING: MD4 is broken and should NOT be used."},
    {"md5",        "WARNING: MD5 is insecure (collision attacks exist)."},
    {"sha1",       "WARNING: SHA-1 is deprecated (practical collision attacks)."},
    {"mdc2",       "WARNING: MDC2 is insecure and obsolete."},
    {"ripemd160",  "Note: RIPEMD-160 has theoretical weaknesses, use with caution."}
};

// ----------------------------------------------------------------------
// OpenSSL MD cache (thread-safe)
// ----------------------------------------------------------------------
static std::unordered_map<std::string, const EVP_MD*> md_cache;
static std::unordered_map<std::string, EVP_MD*> fetched_md_cache;
static std::mutex md_cache_mutex;
static std::once_flag md_cleanup_registered;

void cleanup_fetched_md() {
    std::lock_guard<std::mutex> lock(md_cache_mutex);
    for (auto& pair : fetched_md_cache) {
        EVP_MD_free(const_cast<EVP_MD*>(pair.second));
    }
    fetched_md_cache.clear();
}

const EVP_MD* get_evp_md(const std::string& algo) {
    std::lock_guard<std::mutex> lock(md_cache_mutex);

    auto it = md_cache.find(algo);
    if (it != md_cache.end()) return it->second;

    const EVP_MD* md = nullptr;

    md = EVP_get_digestbyname(algo.c_str());
    if (md) {
        md_cache[algo] = md;
        return md;
    }

    std::string lower_algo = algo;
    for (auto& c : lower_algo) c = tolower(c);
    md = EVP_get_digestbyname(lower_algo.c_str());
    if (md) {
        md_cache[algo] = md;
        return md;
    }

    if (lower_algo == "keccak224" || lower_algo == "keccak256" ||
        lower_algo == "keccak384" || lower_algo == "keccak512") {
        std::string name;
        if (lower_algo == "keccak224") name = "KECCAK-224";
        else if (lower_algo == "keccak256") name = "KECCAK-256";
        else if (lower_algo == "keccak384") name = "KECCAK-384";
        else if (lower_algo == "keccak512") name = "KECCAK-512";

        EVP_MD* fetched = EVP_MD_fetch(NULL, name.c_str(), NULL);
        if (fetched) {
            md_cache[algo] = fetched;
            fetched_md_cache[algo] = fetched;
            std::call_once(md_cleanup_registered, []() { atexit(cleanup_fetched_md); });
            return fetched;
        }
    }

    static const std::unordered_map<std::string, const char*> alias = {
        {"blake2b512", "BLAKE2B-512"},
        {"blake2s256", "BLAKE2S-256"},
        {"whirlpool", "WHIRLPOOL"},
        {"sm3", "SM3"},
        {"ripemd160", "RIPEMD-160"},
        {"sha3-224", "SHA3-224"},
        {"sha3-256", "SHA3-256"},
        {"sha3-384", "SHA3-384"},
        {"sha3-512", "SHA3-512"},
        {"shake128", "SHAKE128"},
        {"shake256", "SHAKE256"},
        {"md5-sha1", "MD5-SHA1"}
    };
    auto alias_it = alias.find(lower_algo);
    if (alias_it != alias.end()) {
        md = EVP_get_digestbyname(alias_it->second);
        if (md) {
            md_cache[algo] = md;
            return md;
        }
    }

    return nullptr;
}

// ----------------------------------------------------------------------
// HMAC helpers using EVP_MAC (OpenSSL 3.0+)
// ----------------------------------------------------------------------
static bool hmac_init(EVP_MAC_CTX** ctx, const std::string& key, const EVP_MD* md) {
    EVP_MAC* mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (!mac) return false;
    *ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (!*ctx) return false;

    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY,
                  (unsigned char*)key.data(), key.size());
    params[1] = OSSL_PARAM_construct_end();

    // Set digest property
    const char* md_name = EVP_MD_get0_name(md);
    OSSL_PARAM digest_param = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                 (char*)md_name, 0);
    // Combine with key param
    OSSL_PARAM all_params[3];
    all_params[0] = params[0];
    all_params[1] = digest_param;
    all_params[2] = OSSL_PARAM_construct_end();

    if (EVP_MAC_init(*ctx, NULL, 0, all_params) != 1) {
        EVP_MAC_CTX_free(*ctx);
        *ctx = nullptr;
        return false;
    }
    return true;
}

static bool hmac_update(EVP_MAC_CTX* ctx, const unsigned char* data, size_t len) {
    return EVP_MAC_update(ctx, data, len) == 1;
}

static bool hmac_final(EVP_MAC_CTX* ctx, unsigned char* out, size_t* out_len, size_t max_len) {
    return EVP_MAC_final(ctx, out, out_len, max_len) == 1;
}

// ----------------------------------------------------------------------
// Core hashing (file)
// ----------------------------------------------------------------------
struct HashResult {
    std::string input_name;
    std::string hex_digest;
    std::string error_msg;
    bool success = false;
    double time_sec = 0.0;
    uint64_t bytes_processed = 0;
    std::string input_type;
    std::string md5_part;
    std::string sha1_part;
};

static HashResult hash_file(const std::string& filename, const std::string& algo,
                            size_t buffer_size, bool show_progress, bool verbose,
                            int digest_bits, const std::string& hmac_key = "") {
    HashResult result;
    result.input_name = filename;
    result.input_type = "file";

    FileHandle fh;
    if (!fh.open(filename)) {
        result.error_msg = "Cannot open file";
        return result;
    }
    uint64_t file_size = fh.size();
    result.bytes_processed = file_size;

    const EVP_MD* md = get_evp_md(algo);
    if (!md) {
        result.error_msg = "Algorithm not supported";
        return result;
    }

    bool is_shake = (algo == "shake128" || algo == "shake256");
    bool is_md5sha1 = (algo == "md5-sha1");
    bool is_hmac = !hmac_key.empty();

    int digest_len = 0;
    if (is_shake) {
        digest_len = (digest_bits > 0) ? (digest_bits + 7) / 8 : ((algo == "shake128") ? 32 : 64);
        if (digest_len > 512) digest_len = 512;
    } else if (is_md5sha1) {
        digest_len = 36;
    } else {
        digest_len = EVP_MD_size(md);
        if (digest_len <= 0) {
            result.error_msg = "Invalid digest length";
            return result;
        }
    }

    std::unique_ptr<SmartProgressBar> pb;
    if (show_progress) {
        pb = std::make_unique<SmartProgressBar>(file_size, "Hashing " + filename);
    }

    std::vector<char> buf1(buffer_size), buf2(buffer_size);
    size_t bytes_read1 = 0, bytes_read2 = 0;
    bool reading = true, has_data1 = false, has_data2 = false;

    if (is_hmac) {
        // EVP_MAC based HMAC
        EVP_MAC_CTX* hmac_ctx = nullptr;
        if (!hmac_init(&hmac_ctx, hmac_key, md)) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
            else snprintf(errbuf, sizeof(errbuf), "Unknown error");
            result.error_msg = "HMAC init failed: " + std::string(errbuf);
            return result;
        }

        // Read first chunk
        bytes_read1 = fh.read(buf1.data(), buffer_size);
        if (bytes_read1 == 0) {
            std::vector<unsigned char> digest(digest_len);
            size_t outlen = 0;
            if (!hmac_final(hmac_ctx, digest.data(), &outlen, digest_len)) {
                unsigned long err = ERR_get_error();
                char errbuf[256];
                if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
                else snprintf(errbuf, sizeof(errbuf), "Unknown error");
                result.error_msg = "HMAC final failed: " + std::string(errbuf);
                EVP_MAC_CTX_free(hmac_ctx);
                return result;
            }
            EVP_MAC_CTX_free(hmac_ctx);
            result.hex_digest = to_hex(digest.data(), outlen);
            result.success = true;
            return result;
        }
        has_data1 = true;

        std::thread hash_thread([&]() {
            if (has_data1) {
                hmac_update(hmac_ctx, reinterpret_cast<const unsigned char*>(buf1.data()), bytes_read1);
                if (pb) pb->update(bytes_read1);
            }
        });

        auto start_time = std::chrono::steady_clock::now();

        while (reading) {
            bytes_read2 = fh.read(buf2.data(), buffer_size);
            if (bytes_read2 == 0) {
                reading = false;
                has_data2 = false;
            } else {
                has_data2 = true;
            }

            if (hash_thread.joinable()) hash_thread.join();
            if (!reading) break;

            std::swap(buf1, buf2);
            std::swap(bytes_read1, bytes_read2);
            has_data1 = true;
            hash_thread = std::thread([&]() {
                hmac_update(hmac_ctx, reinterpret_cast<const unsigned char*>(buf1.data()), bytes_read1);
                if (pb) pb->update(bytes_read1);
            });
        }

        if (hash_thread.joinable()) hash_thread.join();
        if (pb) pb->finish();

        auto end_time = std::chrono::steady_clock::now();
        result.time_sec = std::chrono::duration<double>(end_time - start_time).count();

        std::vector<unsigned char> digest(digest_len);
        size_t outlen = 0;
        if (!hmac_final(hmac_ctx, digest.data(), &outlen, digest_len)) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
            else snprintf(errbuf, sizeof(errbuf), "Unknown error");
            result.error_msg = "HMAC final failed: " + std::string(errbuf);
            EVP_MAC_CTX_free(hmac_ctx);
            return result;
        }
        EVP_MAC_CTX_free(hmac_ctx);

        result.hex_digest = to_hex(digest.data(), outlen);
        if (is_md5sha1 && verbose) {
            result.md5_part = result.hex_digest.substr(0, 32);
            result.sha1_part = result.hex_digest.substr(32, 40);
        }
        result.success = true;
        return result;
    } else {
        // Non-HMAC path using EVP_Digest
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            result.error_msg = "EVP_MD_CTX_new failed";
            return result;
        }
        if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
            else snprintf(errbuf, sizeof(errbuf), "Unknown error");
            result.error_msg = "EVP_DigestInit_ex: " + std::string(errbuf);
            EVP_MD_CTX_free(ctx);
            return result;
        }

        // Read first chunk
        bytes_read1 = fh.read(buf1.data(), buffer_size);
        if (bytes_read1 == 0) {
            std::vector<unsigned char> digest(digest_len);
            unsigned int outlen = digest_len;
            if (EVP_DigestFinal_ex(ctx, digest.data(), &outlen) != 1) {
                unsigned long err = ERR_get_error();
                char errbuf[256];
                if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
                else snprintf(errbuf, sizeof(errbuf), "Unknown error");
                result.error_msg = "EVP_DigestFinal_ex: " + std::string(errbuf);
                EVP_MD_CTX_free(ctx);
                return result;
            }
            EVP_MD_CTX_free(ctx);
            result.hex_digest = to_hex(digest.data(), outlen);
            result.success = true;
            return result;
        }
        has_data1 = true;

        std::thread hash_thread([&]() {
            if (has_data1) {
                EVP_DigestUpdate(ctx, buf1.data(), bytes_read1);
                if (pb) pb->update(bytes_read1);
            }
        });

        auto start_time = std::chrono::steady_clock::now();

        while (reading) {
            bytes_read2 = fh.read(buf2.data(), buffer_size);
            if (bytes_read2 == 0) {
                reading = false;
                has_data2 = false;
            } else {
                has_data2 = true;
            }

            if (hash_thread.joinable()) hash_thread.join();
            if (!reading) break;

            std::swap(buf1, buf2);
            std::swap(bytes_read1, bytes_read2);
            has_data1 = true;
            hash_thread = std::thread([&]() {
                EVP_DigestUpdate(ctx, buf1.data(), bytes_read1);
                if (pb) pb->update(bytes_read1);
            });
        }

        if (hash_thread.joinable()) hash_thread.join();
        if (pb) pb->finish();

        auto end_time = std::chrono::steady_clock::now();
        result.time_sec = std::chrono::duration<double>(end_time - start_time).count();

        std::vector<unsigned char> digest(digest_len);
        unsigned int outlen = digest_len;
        if (EVP_DigestFinal_ex(ctx, digest.data(), &outlen) != 1) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
            else snprintf(errbuf, sizeof(errbuf), "Unknown error");
            result.error_msg = "EVP_DigestFinal_ex: " + std::string(errbuf);
            EVP_MD_CTX_free(ctx);
            return result;
        }
        EVP_MD_CTX_free(ctx);

        result.hex_digest = to_hex(digest.data(), outlen);
        if (is_md5sha1 && verbose) {
            result.md5_part = result.hex_digest.substr(0, 32);
            result.sha1_part = result.hex_digest.substr(32, 40);
        }
        result.success = true;
        return result;
    }
}

static HashResult hash_string(const std::string& str, const std::string& algo, bool verbose, int digest_bits,
                              const std::string& hmac_key = "") {
    HashResult result;
    result.input_name = str;
    result.input_type = "string";

    const EVP_MD* md = get_evp_md(algo);
    if (!md) {
        result.error_msg = "Algorithm not supported";
        return result;
    }

    bool is_shake = (algo == "shake128" || algo == "shake256");
    bool is_md5sha1 = (algo == "md5-sha1");
    bool is_hmac = !hmac_key.empty();

    int digest_len = 0;
    if (is_shake) {
        digest_len = (digest_bits > 0) ? (digest_bits + 7) / 8 : ((algo == "shake128") ? 32 : 64);
        if (digest_len > 512) digest_len = 512;
    } else if (is_md5sha1) {
        digest_len = 36;
    } else {
        digest_len = EVP_MD_size(md);
        if (digest_len <= 0) {
            result.error_msg = "Invalid digest length";
            return result;
        }
    }

    std::vector<unsigned char> digest(digest_len);
    size_t outlen = 0;

    if (is_hmac) {
        EVP_MAC_CTX* hmac_ctx = nullptr;
        if (!hmac_init(&hmac_ctx, hmac_key, md)) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
            else snprintf(errbuf, sizeof(errbuf), "Unknown error");
            result.error_msg = "HMAC init failed: " + std::string(errbuf);
            return result;
        }
        if (!hmac_update(hmac_ctx, reinterpret_cast<const unsigned char*>(str.data()), str.size()) ||
            !hmac_final(hmac_ctx, digest.data(), &outlen, digest_len)) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
            else snprintf(errbuf, sizeof(errbuf), "Unknown error");
            result.error_msg = "HMAC operation failed: " + std::string(errbuf);
            EVP_MAC_CTX_free(hmac_ctx);
            return result;
        }
        EVP_MAC_CTX_free(hmac_ctx);
    } else {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            result.error_msg = "EVP_MD_CTX_new failed";
            return result;
        }
        if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
            else snprintf(errbuf, sizeof(errbuf), "Unknown error");
            result.error_msg = "EVP_DigestInit_ex: " + std::string(errbuf);
            EVP_MD_CTX_free(ctx);
            return result;
        }
        if (EVP_DigestUpdate(ctx, str.data(), str.size()) != 1) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
            else snprintf(errbuf, sizeof(errbuf), "Unknown error");
            result.error_msg = "EVP_DigestUpdate: " + std::string(errbuf);
            EVP_MD_CTX_free(ctx);
            return result;
        }
        unsigned int outlen_uint = digest_len;
        if (EVP_DigestFinal_ex(ctx, digest.data(), &outlen_uint) != 1) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            if (err) ERR_error_string_n(err, errbuf, sizeof(errbuf));
            else snprintf(errbuf, sizeof(errbuf), "Unknown error");
            result.error_msg = "EVP_DigestFinal_ex: " + std::string(errbuf);
            EVP_MD_CTX_free(ctx);
            return result;
        }
        outlen = outlen_uint;
        EVP_MD_CTX_free(ctx);
    }

    result.hex_digest = to_hex(digest.data(), outlen);
    if (is_md5sha1 && verbose) {
        result.md5_part = result.hex_digest.substr(0, 32);
        result.sha1_part = result.hex_digest.substr(32, 40);
    }
    result.success = true;
    result.bytes_processed = str.size();
    return result;
}

// ----------------------------------------------------------------------
// Output functions
// ----------------------------------------------------------------------
static void print_text_result(const HashResult& r, const std::string& algo, bool verbose) {
    if (!r.success) {
        if (use_color()) std::cerr << COLOR_RED;
        std::cerr << "Error: " << r.input_name << " -> " << r.error_msg;
        if (use_color()) std::cerr << COLOR_RESET;
        std::cerr << std::endl;
        return;
    }
    if (use_color()) std::cout << COLOR_GREEN;
    std::cout << "Hash (" << algo << ") of " << r.input_name << ": ";
    if (use_color()) std::cout << COLOR_CYAN << COLOR_BOLD;
    std::cout << r.hex_digest;
    if (use_color()) std::cout << COLOR_RESET;
    std::cout << std::endl;
    if (verbose && r.bytes_processed > 0) {
        double mb = r.bytes_processed / (1024.0 * 1024.0);
        double speed = (r.time_sec > 0) ? mb / r.time_sec : 0.0;
        if (use_color()) std::cout << COLOR_YELLOW;
        std::cout << "  Time: " << r.time_sec << " s, Speed: "
                  << std::fixed << std::setprecision(2) << speed << " MB/s";
        if (use_color()) std::cout << COLOR_RESET;
        std::cout << std::endl;
    }
    if (verbose && !r.md5_part.empty() && !r.sha1_part.empty()) {
        if (use_color()) std::cout << COLOR_YELLOW;
        std::cout << "  MD5 part: " << r.md5_part << "\n  SHA1 part: " << r.sha1_part;
        if (use_color()) std::cout << COLOR_RESET;
        std::cout << std::endl;
    }
}

static void print_json_output(const std::vector<HashResult>& results, const std::string& algo, bool verbose) {
    std::cout << "{\n";
    std::cout << "  \"version\": \"" << HASHER_VERSION << "\",\n";
    std::cout << "  \"algorithm\": \"" << escape_json(algo) << "\",\n";
    std::cout << "  \"results\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::cout << "    {\n";
        std::cout << "      \"input\": \"" << escape_json(r.input_name) << "\",\n";
        std::cout << "      \"type\": \"" << escape_json(r.input_type) << "\",\n";
        if (r.success) {
            std::cout << "      \"hash\": \"" << r.hex_digest << "\",\n";
            std::cout << "      \"time_sec\": " << std::fixed << std::setprecision(6) << r.time_sec << ",\n";
            double mb = r.bytes_processed / (1024.0 * 1024.0);
            double speed = (r.time_sec > 0) ? mb / r.time_sec : 0.0;
            std::cout << "      \"speed_mb_per_sec\": " << std::fixed << std::setprecision(2) << speed << ",\n";
            std::cout << "      \"bytes_processed\": " << r.bytes_processed << ",\n";
            if (verbose && !r.md5_part.empty() && !r.sha1_part.empty()) {
                std::cout << "      \"md5\": \"" << r.md5_part << "\",\n";
                std::cout << "      \"sha1\": \"" << r.sha1_part << "\",\n";
            }
            std::cout << "      \"success\": true\n";
        } else {
            std::cout << "      \"success\": false,\n";
            std::cout << "      \"error\": \"" << escape_json(r.error_msg) << "\"\n";
        }
        std::cout << "    }" << (i == results.size() - 1 ? "\n" : ",\n");
    }
    std::cout << "  ]\n";
    std::cout << "}\n";
}

static void print_csv_output(const std::vector<HashResult>& results, const std::string& algo, bool verbose) {
    std::cout << "input,type,hash,time_sec,speed_mb_per_sec,bytes_processed,success";
    if (verbose) std::cout << ",md5,sha1";
    std::cout << "\n";
    for (const auto& r : results) {
        std::cout << escape_csv(r.input_name) << ","
                  << escape_csv(r.input_type) << ","
                  << (r.success ? r.hex_digest : "FAILED") << ","
                  << std::fixed << std::setprecision(6) << r.time_sec << ","
                  << std::fixed << std::setprecision(2) << ((r.time_sec > 0) ? (r.bytes_processed/(1024.0*1024.0)/r.time_sec) : 0.0) << ","
                  << r.bytes_processed << ","
                  << (r.success ? "true" : "false");
        if (verbose) {
            if (r.success && !r.md5_part.empty() && !r.sha1_part.empty()) {
                std::cout << "," << r.md5_part << "," << r.sha1_part;
            } else {
                std::cout << ",,";
            }
        }
        std::cout << "\n";
    }
}

// ----------------------------------------------------------------------
// Check mode (verify checksums)
// ----------------------------------------------------------------------
bool verify_hashes(const std::string& check_file, const std::string& algo, bool verbose) {
    std::ifstream infile(check_file);
    if (!infile) {
        std::cerr << "Error: Cannot open checksum file: " << check_file << std::endl;
        return false;
    }

    std::string line;
    int line_num = 0;
    bool all_ok = true;

    while (std::getline(infile, line)) {
        ++line_num;
        if (line.empty() || line[0] == '#') continue;

        size_t sep_pos = line.find_last_of(" \t");
        if (sep_pos == std::string::npos) {
            std::cerr << "Warning: line " << line_num << " malformed, skipping: " << line << std::endl;
            continue;
        }
        std::string hash = line.substr(0, sep_pos);
        std::string fname = line.substr(sep_pos + 1);
        if (!fname.empty() && fname[0] == '*') fname = fname.substr(1);
        while (!fname.empty() && (fname[0] == ' ' || fname[0] == '\t')) fname.erase(0, 1);

        HashResult r = hash_file(fname, algo, 64*1024*1024, false, false, 0, "");
        if (!r.success) {
            if (use_color()) std::cerr << COLOR_RED;
            std::cerr << "FAILED: " << fname << " (cannot compute hash)";
            if (use_color()) std::cerr << COLOR_RESET;
            std::cerr << std::endl;
            all_ok = false;
            continue;
        }

        bool match = (r.hex_digest == hash);
        if (match) {
            if (use_color()) std::cout << COLOR_GREEN;
            std::cout << "OK: " << fname;
            if (use_color()) std::cout << COLOR_RESET;
            std::cout << std::endl;
        } else {
            if (use_color()) std::cerr << COLOR_RED;
            std::cerr << "FAILED: " << fname << " (expected " << hash << ", got " << r.hex_digest << ")";
            if (use_color()) std::cerr << COLOR_RESET;
            std::cerr << std::endl;
            all_ok = false;
        }
    }
    return all_ok;
}

// ----------------------------------------------------------------------
// Generate checksum file
// ----------------------------------------------------------------------
void generate_checksum_file(const std::string& output_file, const std::vector<HashResult>& results) {
    std::ofstream out(output_file);
    if (!out) {
        std::cerr << "Error: Cannot write to " << output_file << std::endl;
        return;
    }
    for (const auto& r : results) {
        if (r.success) {
            out << r.hex_digest << "  " << r.input_name << "\n";
        }
    }
    std::cout << "Checksum file written: " << output_file << std::endl;
}

// ----------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Config cfg;
    cfg.algorithm = get_default_algorithm();

    std::string config_path = get_config_path();
    if (!config_path.empty()) {
        load_config(config_path, cfg);
        ::color_mode = cfg.color_mode;
        ::output_format = cfg.output_format;
    }

    parse_args(argc, argv, cfg);

    if (cfg.show_version) {
        print_version();
        return 0;
    }

    OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    OSSL_PROVIDER_load(NULL, "default");

    if (!cfg.check_file.empty()) {
        bool ok = verify_hashes(cfg.check_file, cfg.algorithm, cfg.verbose);
        return ok ? 0 : 1;
    }

    if (cfg.generate_checksum) {
        std::vector<HashResult> results;
        bool all_ok = true;
        for (const auto& input : cfg.inputs) {
            HashResult r = hash_file(input, cfg.algorithm, cfg.buffer_size, false, cfg.verbose, cfg.digest_bits, "");
            if (!r.success) {
                std::cerr << "Error: Cannot hash " << input << ": " << r.error_msg << std::endl;
                all_ok = false;
                continue;
            }
            results.push_back(std::move(r));
        }
        if (!all_ok) return 1;
        generate_checksum_file(cfg.generate_file, results);
        return 0;
    }

    if (output_format == OutputFormat::TEXT) {
        auto warn_it = warnings.find(cfg.algorithm);
        if (warn_it != warnings.end()) {
            if (use_color()) std::cerr << COLOR_YELLOW;
            std::cerr << "[SECURITY WARNING] " << warn_it->second;
            if (use_color()) std::cerr << COLOR_RESET;
            std::cerr << std::endl;
        }
    } else {
        auto warn_it = warnings.find(cfg.algorithm);
        if (warn_it != warnings.end()) {
            std::cerr << "[SECURITY WARNING] " << warn_it->second << std::endl;
        }
    }

    bool is_file_mode = cfg.force_file;
    bool is_string_mode = cfg.force_string;
    if (!is_file_mode && !is_string_mode) {
        std::ifstream test(cfg.inputs[0], std::ios::binary);
        if (test.good()) {
            test.close();
            is_file_mode = true;
            if (cfg.verbose && output_format == OutputFormat::TEXT)
                std::cerr << "Auto-detected inputs as files." << std::endl;
        } else {
            is_string_mode = true;
            if (cfg.verbose && output_format == OutputFormat::TEXT)
                std::cerr << "Auto-detected inputs as strings." << std::endl;
        }
    }

    size_t num_inputs = cfg.inputs.size();
    bool is_single_file = (num_inputs == 1 && is_file_mode);

    bool enable_progress = false;
    if (is_single_file) {
        if (cfg.show_progress) {
            enable_progress = true;
        } else if (!cfg.no_progress) {
            std::ifstream size_test(cfg.inputs[0], std::ios::binary | std::ios::ate);
            if (size_test.good()) {
                uint64_t file_size = size_test.tellg();
                if (file_size > 100ULL * 1024 * 1024) {
                    enable_progress = true;
                }
                size_test.close();
            }
        }
    }

    std::vector<HashResult> all_results;
    size_t batch_size = static_cast<size_t>(std::max(1, cfg.num_threads));

    for (size_t start = 0; start < num_inputs; start += batch_size) {
        size_t end = std::min(start + batch_size, num_inputs);
        size_t batch_count = end - start;
        std::vector<std::thread> threads(batch_count);
        std::vector<HashResult> batch_results(batch_count);

        for (size_t i = 0; i < batch_count; ++i) {
            size_t idx = start + i;
            threads[i] = std::thread([&, idx]() {
                if (is_file_mode) {
                    bool prog = (num_inputs == 1) ? enable_progress : false;
                    batch_results[idx - start] = hash_file(cfg.inputs[idx], cfg.algorithm, cfg.buffer_size,
                                             prog, cfg.verbose, cfg.digest_bits, cfg.hmac_key);
                } else {
                    batch_results[idx - start] = hash_string(cfg.inputs[idx], cfg.algorithm, cfg.verbose, cfg.digest_bits, cfg.hmac_key);
                }
            });
        }
        for (auto& t : threads) t.join();

        for (auto& r : batch_results) {
            all_results.push_back(std::move(r));
        }

        if (output_format == OutputFormat::TEXT) {
            for (size_t i = 0; i < batch_count; ++i) {
                size_t idx = start + i;
                if (cfg.verbose && num_inputs > 1) {
                    std::cout << "[" << (idx+1) << "/" << num_inputs << "] ";
                }
                print_text_result(all_results[idx], cfg.algorithm, cfg.verbose);
            }
        }
    }

    if (output_format == OutputFormat::JSON) {
        print_json_output(all_results, cfg.algorithm, cfg.verbose);
    } else if (output_format == OutputFormat::CSV) {
        print_csv_output(all_results, cfg.algorithm, cfg.verbose);
    }

    for (auto& r : all_results) {
        if (!r.success) return 1;
    }
    return 0;
}