// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strict field reader for render-script objects (docs/math/00-conventions.md C9, doc 10 §11,
// doc 20 A9 / "Validation is strict", doc 30 §19, doc 40 §2.0).
//
// Every accessor marks its key as consumed. finish() then rejects any key that was never read, so
// a typo can never fall back to a default. A wrong JSON type, a missing required key or a value
// outside its stated range throws rl::ScriptError. Values are never clamped.
//
// Types:
//   int    - a JSON integer token (nlohmann number_integer / number_unsigned). A number written with
//            a fraction or exponent ("3.0", "1e2") is NOT an int, even if integral.
//   double - any JSON number, parsed to the nearest binary64 (nlohmann uses strtod).
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "core/base/types.hpp"

namespace rl::script {

using Json = nlohmann::json;

// [x, y, w, h] integers, w, h >= 1 (doc 10 §11).
struct Rect {
    int64_t x = 0;
    int64_t y = 0;
    int64_t w = 0;
    int64_t h = 0;
};

class Fields {
public:
    // `obj` must be a JSON object and must outlive this reader. `context` prefixes error messages,
    // e.g. "op #3 (add_layer)".
    Fields(const Json& obj, std::string context);

    const std::string& context() const { return ctx_; }
    bool has(const char* key) const;

    // Marks `key` consumed without reading it (e.g. the "op" discriminator).
    void consume(const char* key);

    // Raw access for lane-specific shapes (arrays of samples, points, matrices). Marks consumed.
    const Json* raw(const char* key);
    const Json& raw_req(const char* key);

    std::string req_string(const char* key);
    std::string string_or(const char* key, const std::string& def);
    // A string restricted to `allowed`.
    std::string req_enum(const char* key, std::initializer_list<const char*> allowed);
    std::string enum_or(const char* key, const std::string& def, std::initializer_list<const char*> allowed);

    int64_t req_int(const char* key, int64_t lo, int64_t hi);
    int64_t int_or(const char* key, int64_t def, int64_t lo, int64_t hi);
    std::optional<int64_t> opt_int(const char* key, int64_t lo, int64_t hi);

    double req_double(const char* key, double lo, double hi);
    double double_or(const char* key, double def, double lo, double hi);

    bool req_bool(const char* key);
    bool bool_or(const char* key, bool def);

    // "#RRGGBB" / "#RRGGBBAA"; NOT canonicalised.
    Rgba8 req_color(const char* key);
    Rgba8 color_or(const char* key, Rgba8 def);

    // [x, y, w, h] with w, h >= 1.
    Rect rect_or(const char* key, Rect def);

    // Rejects every key of the object that no accessor consumed. Call after reading all fields and
    // before changing the document.
    void finish();

    // Throws ScriptError("<context>: <msg>").
    [[noreturn]] void fail(const std::string& msg) const;

    // Helpers usable on any JSON value (element of an array, etc.).
    [[noreturn]] static void fail_at(const std::string& context, const std::string& msg);
    static int64_t as_int(const Json& v, const std::string& what, int64_t lo, int64_t hi);
    static double as_double(const Json& v, const std::string& what, double lo, double hi);

private:
    const Json* find(const char* key);

    const Json& obj_;
    std::string ctx_;
    std::set<std::string> consumed_;
};

// Seeds: JSON integers in [0, 2^53) (doc 10 §11).
constexpr int64_t kMaxSeedExclusive = int64_t{1} << 53;

}  // namespace rl::script
