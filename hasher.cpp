// hasher.cpp – High-performance multi-algorithm hash calculator
// Version: 1.0
// Requires OpenSSL 3.x
// Compile: g++ -std=c++17 -O3 -pthread -o hasher hasher.cpp -lcrypto -lssl
// Usage:   hasher [options] <input> [algorithm] [<input2> ...]

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
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/provider.h>
#include <openssl/opensslv.h>

// ----------------------------------------------------------------------
// Version
// ----------------------------------------------------------------------
#define HASHER_VERSION "1.0"

static void print_version() {
    std::cout << "hasher version " << HASHER_VERSION << "\n"
              << "OpenSSL: " << OPENSSL_VERSION_TEXT << "\n"
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
// Config
// ----------------------------------------------------------------------
struct Config {
    bool show_progress = false;
    bool no_progress = false;
    bool verbose = false;
    bool force_file = false;
    bool force_string = false;
    size_t buffer_size = 64 * 1024 * 1024; // 64 MB
    int num_threads = 1;
    std::string algorithm = "sha256";
    std::vector<std::string> inputs;
    int digest_bits = 0;
    bool show_version = false;
};

// ----------------------------------------------------------------------
// Parse args
// ----------------------------------------------------------------------
Config parse_args(int argc, char* argv[]) {
    Config cfg;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        std::cout << "Usage: hasher [OPTIONS] <input> [algorithm] [<input2> ...]\n"
                  << "  If multiple inputs are given, they are hashed in parallel (--threads).\n"
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
                  << "  Default algorithm: sha256\n";
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
        else if (a[0] == '-') {
            std::cerr << "Unknown option: " << a << std::endl;
            exit(1);
        }
        else {
            positional.push_back(a);
        }
    }

    if (positional.empty() && !cfg.show_version) {
        std::cerr << "Error: Missing input.\n";
        exit(1);
    }

    if (positional.size() >= 2) {
        cfg.algorithm = positional.back();
        for (auto& c : cfg.algorithm) c = tolower(c);
        for (size_t i = 0; i < positional.size() - 1; ++i)
            cfg.inputs.push_back(positional[i]);
    } else if (!positional.empty()) {
        cfg.inputs = {positional[0]};
    }

    return cfg;
}

// ----------------------------------------------------------------------
// Progress bar (only for single-file mode)
// ----------------------------------------------------------------------
class ProgressBar {
public:
    ProgressBar(uint64_t total, const std::string& label = "")
        : total_(total), label_(label), done_(0), last_pct_(-1) {}

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
    std::mutex mtx_;

    void print(int pct) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::cerr << "\r" << label_ << " [" << std::string(pct/2, '=') 
                  << (pct < 100 ? ">" : "") 
                  << std::string(50 - pct/2 - (pct < 100 ? 1 : 0), ' ') 
                  << "] " << pct << "%" << std::flush;
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

    // Keccak
    if (algo == "keccak224" || algo == "keccak256" ||
        algo == "keccak384" || algo == "keccak512") {
        std::string name;
        if (algo == "keccak224") name = "KECCAK-224";
        else if (algo == "keccak256") name = "KECCAK-256";
        else if (algo == "keccak384") name = "KECCAK-384";
        else if (algo == "keccak512") name = "KECCAK-512";

        EVP_MD* fetched = EVP_MD_fetch(NULL, name.c_str(), NULL);
        if (fetched) {
            md_cache[algo] = fetched;
            fetched_md_cache[algo] = fetched;
            std::call_once(md_cleanup_registered, []() { atexit(cleanup_fetched_md); });
            return fetched;
        }
    }

    // MD5-SHA1
    if (algo == "md5-sha1") {
        md = EVP_get_digestbyname("MD5-SHA1");
        if (md) {
            md_cache[algo] = md;
            return md;
        }
    }

    // Aliases
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
        {"shake256", "SHAKE256"}
    };
    auto alias_it = alias.find(algo);
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
// FileHandle using std::ifstream with large buffer
// ----------------------------------------------------------------------
class FileHandle {
public:
    FileHandle() : file_(), buffer_() {}
    ~FileHandle() { close(); }

    bool open(const std::string& path, size_t buffer_size) {
        file_.open(path, std::ios::binary);
        if (!file_.is_open()) return false;
        // Set custom buffer
        buffer_.resize(buffer_size);
        file_.rdbuf()->pubsetbuf(buffer_.data(), buffer_.size());
        return true;
    }

    size_t read(void* buf, size_t count) {
        if (!file_.is_open()) return 0;
        file_.read(static_cast<char*>(buf), count);
        return file_.gcount();
    }

    void close() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    uint64_t size() {
        if (!file_.is_open()) return 0;
        auto pos = file_.tellg();
        file_.seekg(0, std::ios::end);
        uint64_t sz = file_.tellg();
        file_.seekg(pos);
        return sz;
    }

    bool is_open() const { return file_.is_open(); }

private:
    std::ifstream file_;
    std::vector<char> buffer_;
};

// ----------------------------------------------------------------------
// HashResult
// ----------------------------------------------------------------------
struct HashResult {
    std::string input_name;
    std::string hex_digest;
    std::string error_msg;
    bool success = false;
    double time_sec = 0.0;
    uint64_t bytes_processed = 0;
};

// ----------------------------------------------------------------------
// File hashing
// ----------------------------------------------------------------------
HashResult hash_file(const std::string& filename, const std::string& algo,
                     size_t buffer_size, bool show_progress, bool verbose,
                     int digest_bits) {
    HashResult result;
    result.input_name = filename;

    FileHandle fh;
    if (!fh.open(filename, buffer_size)) {
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

    std::unique_ptr<ProgressBar> pb;
    if (show_progress) {
        pb = std::make_unique<ProgressBar>(file_size, "Hashing " + filename);
    }

    // Double-buffering
    std::vector<char> buf1(buffer_size), buf2(buffer_size);
    size_t bytes_read1 = 0, bytes_read2 = 0;
    bool reading = true, has_data1 = false, has_data2 = false;

    bytes_read1 = fh.read(buf1.data(), buffer_size);
    if (bytes_read1 == 0) {
        // Empty file
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
    result.success = true;
    return result;
}

// ----------------------------------------------------------------------
// String hashing
// ----------------------------------------------------------------------
HashResult hash_string(const std::string& str, const std::string& algo, bool verbose, int digest_bits) {
    HashResult result;
    result.input_name = str;

    const EVP_MD* md = get_evp_md(algo);
    if (!md) {
        result.error_msg = "Algorithm not supported";
        return result;
    }

    bool is_shake = (algo == "shake128" || algo == "shake256");
    bool is_md5sha1 = (algo == "md5-sha1");

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
    result.bytes_processed = str.size();
    return result;
}

// ----------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);

    if (cfg.show_version) {
        print_version();
        return 0;
    }

    OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    OSSL_PROVIDER_load(NULL, "default");

    auto warn_it = warnings.find(cfg.algorithm);
    if (warn_it != warnings.end()) {
        std::cerr << "[SECURITY WARNING] " << warn_it->second << std::endl;
    }

    bool is_file_mode = cfg.force_file;
    bool is_string_mode = cfg.force_string;
    if (!is_file_mode && !is_string_mode) {
        std::ifstream test(cfg.inputs[0], std::ios::binary);
        if (test.good()) {
            test.close();
            is_file_mode = true;
            if (cfg.verbose) std::cerr << "Auto-detected inputs as files." << std::endl;
        } else {
            is_string_mode = true;
            if (cfg.verbose) std::cerr << "Auto-detected inputs as strings." << std::endl;
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

    std::vector<HashResult> results(num_inputs);
    size_t batch_size = static_cast<size_t>(std::max(1, cfg.num_threads));

    for (size_t start = 0; start < num_inputs; start += batch_size) {
        size_t end = std::min(start + batch_size, num_inputs);
        size_t batch_count = end - start;
        std::vector<std::thread> threads(batch_count);

        for (size_t i = 0; i < batch_count; ++i) {
            size_t idx = start + i;
            threads[i] = std::thread([&, idx]() {
                if (is_file_mode) {
                    bool prog = (num_inputs == 1) ? enable_progress : false;
                    results[idx] = hash_file(cfg.inputs[idx], cfg.algorithm, cfg.buffer_size,
                                             prog, cfg.verbose, cfg.digest_bits);
                } else {
                    results[idx] = hash_string(cfg.inputs[idx], cfg.algorithm, cfg.verbose, cfg.digest_bits);
                }
            });
        }
        for (auto& t : threads) t.join();

        for (size_t i = 0; i < batch_count; ++i) {
            size_t idx = start + i;
            if (results[idx].success) {
                std::cout << "[" << (idx+1) << "/" << num_inputs << "] "
                          << results[idx].input_name << " -> "
                          << results[idx].hex_digest;
                if (cfg.verbose && results[idx].bytes_processed > 0) {
                    double mb = results[idx].bytes_processed / (1024.0 * 1024.0);
                    double speed = mb / results[idx].time_sec;
                    std::cout << " (Time: " << results[idx].time_sec << "s, Speed: "
                              << std::fixed << std::setprecision(2) << speed << " MB/s)";
                }
                std::cout << std::endl;
                if (cfg.verbose && cfg.algorithm == "md5-sha1") {
                    std::string md5_part = results[idx].hex_digest.substr(0, 32);
                    std::string sha1_part = results[idx].hex_digest.substr(32, 40);
                    std::cerr << "    MD5 part: " << md5_part << "\n    SHA1 part: " << sha1_part << std::endl;
                }
            } else {
                std::cerr << "[" << (idx+1) << "/" << num_inputs << "] "
                          << results[idx].input_name << " FAILED: "
                          << results[idx].error_msg << std::endl;
            }
        }
    }

    for (auto& r : results) {
        if (!r.success) return 1;
    }
    return 0;
}