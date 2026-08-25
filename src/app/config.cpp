#include "app/config.hpp"

#include <sys/stat.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>
#include <variant>
#include <vector>

#include "core/log.hpp"
#include "core/units.hpp"

namespace pc::app {
namespace {

// ------------------------------------------------------------------------------------------
// A minimal, hand-rolled JSON reader, scoped to exactly what this file needs: objects,
// arrays of strings, strings, numbers, and bools. Not a general-purpose JSON library --
// nlohmann/json is fetched by cmake/Dependencies.cmake but is not currently linked into any
// buildable target (it is not on parsec_core's or parsec_tests' target_link_libraries list),
// and CMakeLists.txt is out of scope for this change. Pulling in a real dependency would mean
// either editing the build file this task explicitly leaves alone, or silently failing to
// compile -- neither is acceptable, so config parsing is self-contained instead. The format
// this reads/writes is deliberately simple (a flat object of scalars) so a
// hand-rolled parser is a reasonable size, not a liability.
// ------------------------------------------------------------------------------------------

struct JVal;
using JObject = std::map<std::string, JVal>;
using JArray = std::vector<JVal>;

struct JVal {
    std::variant<std::monostate, bool, double, std::string, JArray, JObject> v;

    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<JObject>(v); }
    [[nodiscard]] const JObject* as_object() const noexcept {
        return std::holds_alternative<JObject>(v) ? &std::get<JObject>(v) : nullptr;
    }
    [[nodiscard]] const JArray* as_array() const noexcept {
        return std::holds_alternative<JArray>(v) ? &std::get<JArray>(v) : nullptr;
    }
    [[nodiscard]] const std::string* as_string() const noexcept {
        return std::holds_alternative<std::string>(v) ? &std::get<std::string>(v) : nullptr;
    }
    [[nodiscard]] const double* as_number() const noexcept {
        return std::holds_alternative<double>(v) ? &std::get<double>(v) : nullptr;
    }
    [[nodiscard]] const bool* as_bool() const noexcept {
        return std::holds_alternative<bool>(v) ? &std::get<bool>(v) : nullptr;
    }
};

// Simple recursive-descent parser over a std::string_view. Throws std::runtime_error on any
// malformed input; every call site below catches it and falls back to defaults rather than
// letting a bad config.json propagate into the engine (see Config::load).
class JsonParser {
public:
    explicit JsonParser(std::string_view s) : s_(s) {}

    JVal parse() {
        skip_ws();
        JVal result = parse_value();
        skip_ws();
        // Trailing garbage is tolerated rather than rejected -- a config file with a stray
        // comment or extra newline at the end should not lose every field over one character.
        return result;
    }

private:
    std::string_view s_;
    size_t i_{};

    [[noreturn]] void fail(const char* why) { throw std::runtime_error(why); }

    [[nodiscard]] bool eof() const noexcept { return i_ >= s_.size(); }
    [[nodiscard]] char peek() const { return eof() ? '\0' : s_[i_]; }
    char take() {
        if (eof())
            fail("unexpected end of input");
        return s_[i_++];
    }
    void skip_ws() {
        while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
            ++i_;
    }
    void expect(char c) {
        if (take() != c)
            fail("unexpected character");
    }

    JVal parse_value() {
        skip_ws();
        switch (peek()) {
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case '"':
                return JVal{parse_string()};
            case 't':
                expect_literal("true");
                return JVal{true};
            case 'f':
                expect_literal("false");
                return JVal{false};
            case 'n':
                expect_literal("null");
                return JVal{};
            default:
                return JVal{parse_number()};
        }
    }

    void expect_literal(const char* lit) {
        const size_t n = std::strlen(lit);
        if (i_ + n > s_.size() || s_.compare(i_, n, lit) != 0)
            fail("invalid literal");
        i_ += n;
    }

    JVal parse_object() {
        expect('{');
        JObject obj;
        skip_ws();
        if (peek() == '}') {
            ++i_;
            return JVal{obj};
        }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            obj[key] = parse_value();
            skip_ws();
            const char c = take();
            if (c == '}')
                break;
            if (c != ',')
                fail("expected ',' or '}' in object");
        }
        return JVal{obj};
    }

    JVal parse_array() {
        expect('[');
        JArray arr;
        skip_ws();
        if (peek() == ']') {
            ++i_;
            return JVal{arr};
        }
        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            const char c = take();
            if (c == ']')
                break;
            if (c != ',')
                fail("expected ',' or ']' in array");
        }
        return JVal{arr};
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            const char c = take();
            if (c == '"')
                break;
            if (c == '\\') {
                const char esc = take();
                switch (esc) {
                    case '"':
                        out.push_back('"');
                        break;
                    case '\\':
                        out.push_back('\\');
                        break;
                    case '/':
                        out.push_back('/');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 'u':
                        // \uXXXX is accepted syntactically (skip 4 hex digits) but not decoded
                        // -- nothing this file reads needs non-ASCII (coin names, addresses,
                        // hex, ASCII paths), so a literal '?' is a safe, honest placeholder
                        // rather than pulling in UTF-16 surrogate-pair handling for no reason.
                        for (int k = 0; k < 4; ++k)
                            take();
                        out.push_back('?');
                        break;
                    default:
                        fail("invalid escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    double parse_number() {
        const size_t start = i_;
        if (peek() == '-')
            ++i_;
        while (!eof() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.' ||
                          peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-'))
            ++i_;
        if (i_ == start)
            fail("invalid number");
        char* end = nullptr;
        const std::string tok(s_.substr(start, i_ - start));
        const double val = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str())
            fail("invalid number");
        return val;
    }
};

// Best-effort field extraction: missing key or wrong type leaves `out` untouched (caller has
// already set it to the default), rather than aborting the rest of the object.
void get_bool(const JObject& obj, const char* key, bool& out) noexcept {
    const auto it = obj.find(key);
    if (it != obj.end())
        if (const bool* b = it->second.as_bool())
            out = *b;
}
void get_string(const JObject& obj, const char* key, std::string& out) noexcept {
    const auto it = obj.find(key);
    if (it != obj.end())
        if (const std::string* s = it->second.as_string())
            out = *s;
}
void get_i64(const JObject& obj, const char* key, int64_t& out) noexcept {
    const auto it = obj.find(key);
    if (it != obj.end())
        if (const double* n = it->second.as_number())
            out = static_cast<int64_t>(*n);
}
void get_u64(const JObject& obj, const char* key, uint64_t& out) noexcept {
    const auto it = obj.find(key);
    if (it != obj.end())
        if (const double* n = it->second.as_number())
            if (*n >= 0)
                out = static_cast<uint64_t>(*n);
}
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out.push_back(c);
        }
    }
    return out;
}

std::string home_dir() {
    if (const char* home = std::getenv("HOME"))
        return home;
    return ".";  // never crash on a missing environment; falls back to the cwd
}

}  // namespace

Config Config::defaults() noexcept {
    Config cfg{};
    cfg.mainnet = true;
    cfg.keystore_path = default_keystore_path(true);
    return cfg;
}

std::string Config::default_keystore_path(bool mainnet) noexcept {
    return home_dir() + (mainnet ? "/.parsec/keystore-mainnet.json"
                                 : "/.parsec/keystore-testnet.json");
}

std::string Config::default_path() noexcept {
    return home_dir() + "/.parsec/config.json";
}

Config Config::load(const std::string& path) noexcept {
    Config cfg = defaults();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        // No config file is the expected, common case (fresh install, read-only market-data
        // mode) -- not an error worth a log line.
        return cfg;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();

    try {
        JsonParser parser(text);
        const JVal root = parser.parse();
        const JObject* obj = root.as_object();
        if (!obj) {
            PC_LOG_WARN("config: %s is not a JSON object, using defaults", path.c_str());
            return cfg;
        }

        get_bool(*obj, "mainnet", cfg.mainnet);
        bool has_keystore_path = false;
        if (const auto it = obj->find("keystore_path"); it != obj->end()) {
            if (const std::string* path_value = it->second.as_string()) {
                cfg.keystore_path = *path_value;
                has_keystore_path = true;
            }
        }
        if (!has_keystore_path)
            cfg.keystore_path = default_keystore_path(cfg.mainnet);
        get_string(*obj, "master_address", cfg.master_address);
        get_u64(*obj, "order_ack_timeout_ms", cfg.order_ack_timeout_ms);
        get_u64(*obj, "dms_heartbeat_interval_ms", cfg.dms_heartbeat_interval_ms);
        {
            int64_t slip = cfg.default_slippage_bps;
            get_i64(*obj, "default_slippage_bps", slip);
            cfg.default_slippage_bps = static_cast<int32_t>(slip);
        }
    } catch (const std::exception& e) {
        // Malformed JSON must never throw into the engine -- fall back to defaults() field for
        // field (whatever parsed before the failure is discarded; cfg is still the pristine
        // defaults() at this point since every get_* above only ever narrows a valid parse).
        PC_LOG_WARN("config: failed to parse %s (%s), using defaults", path.c_str(), e.what());
        return defaults();
    }

    return cfg;
}

bool Config::save(const std::string& path) const noexcept {
    // Best-effort directory creation, mode 0700 to match the keystore directory rule (docs/06
    // §5.4). mkdir returning EEXIST is not an error -- the common case is the directory already
    // exists from a previous run.
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        const std::string dir = path.substr(0, slash);
        if (!dir.empty() && ::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
            return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;

    file << "{\n";
    file << "  \"mainnet\": " << (mainnet ? "true" : "false") << ",\n";
    file << "  \"keystore_path\": \"" << json_escape(keystore_path) << "\",\n";
    file << "  \"master_address\": \"" << json_escape(master_address) << "\",\n";
    file << "  \"order_ack_timeout_ms\": " << order_ack_timeout_ms << ",\n";
    file << "  \"dms_heartbeat_interval_ms\": " << dms_heartbeat_interval_ms << ",\n";
    file << "  \"default_slippage_bps\": " << default_slippage_bps << "\n";
    file << "}\n";

    return file.good();
}


}  // namespace pc::app
