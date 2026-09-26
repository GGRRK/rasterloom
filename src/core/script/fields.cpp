// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/script/fields.hpp"

#include <cmath>
#include <limits>
#include <sstream>

#include "core/base/color.hpp"
#include "core/base/error.hpp"

namespace rl::script {

namespace {
std::string fmt_double(double v) {
    std::ostringstream os;
    os.precision(17);
    os << v;
    return os.str();
}
}  // namespace

Fields::Fields(const Json& obj, std::string context) : obj_(obj), ctx_(std::move(context)) {
    if (!obj_.is_object()) fail("expected a JSON object");
}

void Fields::fail(const std::string& msg) const { fail_at(ctx_, msg); }

void Fields::fail_at(const std::string& context, const std::string& msg) {
    throw ScriptError(context + ": " + msg);
}

bool Fields::has(const char* key) const { return obj_.contains(key); }

void Fields::consume(const char* key) { consumed_.insert(key); }

const Json* Fields::find(const char* key) {
    consumed_.insert(key);
    auto it = obj_.find(key);
    return it == obj_.end() ? nullptr : &*it;
}

const Json* Fields::raw(const char* key) { return find(key); }

const Json& Fields::raw_req(const char* key) {
    const Json* v = find(key);
    if (!v) fail(std::string("missing required field '") + key + "'");
    return *v;
}

std::string Fields::req_string(const char* key) {
    const Json& v = raw_req(key);
    if (!v.is_string()) fail(std::string("field '") + key + "' must be a string");
    return v.get<std::string>();
}

std::string Fields::string_or(const char* key, const std::string& def) {
    const Json* v = find(key);
    if (!v) return def;
    if (!v->is_string()) fail(std::string("field '") + key + "' must be a string");
    return v->get<std::string>();
}

static std::string check_enum(const Fields& f, const char* key, const std::string& s,
                              std::initializer_list<const char*> allowed) {
    for (const char* a : allowed)
        if (s == a) return s;
    std::string list;
    for (const char* a : allowed) list += std::string(list.empty() ? "" : ", ") + "'" + a + "'";
    f.fail(std::string("field '") + key + "' has invalid value '" + s + "' (allowed: " + list + ")");
}

std::string Fields::req_enum(const char* key, std::initializer_list<const char*> allowed) {
    return check_enum(*this, key, req_string(key), allowed);
}

std::string Fields::enum_or(const char* key, const std::string& def, std::initializer_list<const char*> allowed) {
    if (!has(key)) {
        consume(key);
        return def;
    }
    return check_enum(*this, key, string_or(key, def), allowed);
}

int64_t Fields::as_int(const Json& v, const std::string& what, int64_t lo, int64_t hi) {
    int64_t out = 0;
    if (v.is_number_unsigned()) {
        const uint64_t u = v.get<uint64_t>();
        if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            throw ScriptError(what + " out of range [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
        out = static_cast<int64_t>(u);
    } else if (v.is_number_integer()) {
        out = v.get<int64_t>();
    } else {
        throw ScriptError(what + " must be a JSON integer");
    }
    if (out < lo || out > hi)
        throw ScriptError(what + " = " + std::to_string(out) + " out of range [" + std::to_string(lo) + ", " +
                          std::to_string(hi) + "]");
    return out;
}

double Fields::as_double(const Json& v, const std::string& what, double lo, double hi) {
    if (!v.is_number()) throw ScriptError(what + " must be a number");
    double d = 0.0;
    if (v.is_number_float()) {
        d = v.get<double>();
    } else if (v.is_number_unsigned()) {
        d = static_cast<double>(v.get<uint64_t>());
    } else {
        d = static_cast<double>(v.get<int64_t>());
    }
    if (!std::isfinite(d)) throw ScriptError(what + " must be finite");
    if (!(d >= lo && d <= hi))
        throw ScriptError(what + " = " + fmt_double(d) + " out of range [" + fmt_double(lo) + ", " +
                          fmt_double(hi) + "]");
    return d;
}

int64_t Fields::req_int(const char* key, int64_t lo, int64_t hi) {
    return as_int(raw_req(key), ctx_ + ": field '" + key + "'", lo, hi);
}

int64_t Fields::int_or(const char* key, int64_t def, int64_t lo, int64_t hi) {
    const Json* v = find(key);
    if (!v) return def;
    return as_int(*v, ctx_ + ": field '" + key + "'", lo, hi);
}

std::optional<int64_t> Fields::opt_int(const char* key, int64_t lo, int64_t hi) {
    const Json* v = find(key);
    if (!v) return std::nullopt;
    return as_int(*v, ctx_ + ": field '" + key + "'", lo, hi);
}

double Fields::req_double(const char* key, double lo, double hi) {
    return as_double(raw_req(key), ctx_ + ": field '" + key + "'", lo, hi);
}

double Fields::double_or(const char* key, double def, double lo, double hi) {
    const Json* v = find(key);
    if (!v) return def;
    return as_double(*v, ctx_ + ": field '" + key + "'", lo, hi);
}

bool Fields::req_bool(const char* key) {
    const Json& v = raw_req(key);
    if (!v.is_boolean()) fail(std::string("field '") + key + "' must be a boolean");
    return v.get<bool>();
}

bool Fields::bool_or(const char* key, bool def) {
    const Json* v = find(key);
    if (!v) return def;
    if (!v->is_boolean()) fail(std::string("field '") + key + "' must be a boolean");
    return v->get<bool>();
}

Rgba8 Fields::req_color(const char* key) {
    const std::string s = req_string(key);
    auto c = parse_hex_color(s);
    if (!c) fail(std::string("field '") + key + "' is not a colour \"#RRGGBB\" or \"#RRGGBBAA\": '" + s + "'");
    return *c;
}

Rgba8 Fields::color_or(const char* key, Rgba8 def) {
    if (!has(key)) {
        consume(key);
        return def;
    }
    return req_color(key);
}

Rect Fields::rect_or(const char* key, Rect def) {
    const Json* v = find(key);
    if (!v) return def;
    if (!v->is_array() || v->size() != 4) fail(std::string("field '") + key + "' must be an array [x, y, w, h]");
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min() / 4;
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max() / 4;
    const std::string what = ctx_ + ": field '" + key + "'";
    Rect r;
    r.x = as_int((*v)[0], what + "[0]", kMin, kMax);
    r.y = as_int((*v)[1], what + "[1]", kMin, kMax);
    r.w = as_int((*v)[2], what + "[2] (w)", 1, kMax);
    r.h = as_int((*v)[3], what + "[3] (h)", 1, kMax);
    return r;
}

void Fields::finish() {
    for (auto it = obj_.begin(); it != obj_.end(); ++it) {
        if (consumed_.count(it.key()) == 0) fail("unknown field '" + it.key() + "'");
    }
}

}  // namespace rl::script
