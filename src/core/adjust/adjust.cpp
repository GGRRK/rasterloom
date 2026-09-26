// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adjustment dispatch: render-script params parsing (doc 20 A1-A8 tables, strict validation), the
// typed builders, and params_to_json.
#include <cstdio>
#include <nlohmann/json.hpp>

#include "core/adjust/adjust_math.hpp"
#include "core/adjust/adjustment.hpp"
#include "core/base/color.hpp"
#include "core/base/error.hpp"
#include "core/script/fields.hpp"

namespace rl::adjust {

namespace {

constexpr const char* kKnownTypes[] = {
    "levels", "curves", "brightness_contrast", "hue_saturation",
    "black_white", "invert", "posterize", "threshold",
};

using script::Fields;
using Json = nlohmann::json;

[[noreturn]] void fail(const std::string& ctx, const std::string& msg) { throw ScriptError(ctx + ": " + msg); }

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

void check_int(const std::string& ctx, const char* what, int v, int lo, int hi) {
    if (v < lo || v > hi)
        fail(ctx, std::string(what) + " = " + std::to_string(v) + " out of range [" + std::to_string(lo) + ", " +
                      std::to_string(hi) + "]");
}

void check_double(const std::string& ctx, const char* what, double v, double lo, double hi) {
    if (!(v >= lo && v <= hi)) fail(ctx, std::string(what) + " = " + num(v) + " out of range [" + num(lo) + ", " + num(hi) + "]");
}

// ---- validation of typed params (A1-A8 ranges) ----------------------------------------------------
void validate_setting(const std::string& ctx, const LevelsSetting& s) {
    check_int(ctx, "in_black", s.in_black, 0, 254);
    check_int(ctx, "in_white", s.in_white, 1, 255);
    if (!(s.in_black < s.in_white)) fail(ctx, "in_black must be below in_white");
    check_double(ctx, "gamma", s.gamma, 0.1, 9.99);
    check_int(ctx, "out_black", s.out_black, 0, 255);
    check_int(ctx, "out_white", s.out_white, 0, 255);
}

void validate_curve(const std::string& ctx, const CurvePoints& c) {
    if (c.size() < 2 || c.size() > 16) fail(ctx, "a curve needs 2..16 points, got " + std::to_string(c.size()));
    for (size_t i = 0; i < c.size(); ++i) {
        check_int(ctx, "curve point input", c[i][0], 0, 255);
        check_int(ctx, "curve point output", c[i][1], 0, 255);
        if (i > 0 && !(c[i][0] > c[i - 1][0])) fail(ctx, "curve point inputs must be strictly increasing");
    }
}

struct Validator {
    const std::string& ctx;
    void operator()(const LevelsParams& p) const {
        validate_setting(ctx + " rgb", p.rgb);
        validate_setting(ctx + " r", p.r);
        validate_setting(ctx + " g", p.g);
        validate_setting(ctx + " b", p.b);
    }
    void operator()(const CurvesParams& p) const {
        validate_curve(ctx + " rgb", p.rgb);
        validate_curve(ctx + " r", p.r);
        validate_curve(ctx + " g", p.g);
        validate_curve(ctx + " b", p.b);
    }
    void operator()(const BrightnessContrastParams& p) const {
        check_double(ctx, "brightness", p.brightness, -1.0, 1.0);
        check_double(ctx, "contrast", p.contrast, -1.0, 1.0);
    }
    void operator()(const HueSaturationParams& p) const {
        if (p.colorize) {
            check_double(ctx, "hue", p.hue, 0.0, 360.0);
            check_double(ctx, "saturation", p.saturation, 0.0, 100.0);
        } else {
            check_double(ctx, "hue", p.hue, -180.0, 180.0);
            check_double(ctx, "saturation", p.saturation, -100.0, 100.0);
        }
        check_double(ctx, "lightness", p.lightness, -100.0, 100.0);
    }
    void operator()(const BlackWhiteParams& p) const {
        check_double(ctx, "reds", p.reds, -200.0, 300.0);
        check_double(ctx, "yellows", p.yellows, -200.0, 300.0);
        check_double(ctx, "greens", p.greens, -200.0, 300.0);
        check_double(ctx, "cyans", p.cyans, -200.0, 300.0);
        check_double(ctx, "blues", p.blues, -200.0, 300.0);
        check_double(ctx, "magentas", p.magentas, -200.0, 300.0);
    }
    void operator()(const InvertParams&) const {}
    void operator()(const PosterizeParams& p) const { check_int(ctx, "levels", p.levels, 2, 255); }
    void operator()(const ThresholdParams& p) const { check_int(ctx, "level", p.level, 1, 255); }
};

// ---- the three channel-mixing adjustments ----------------------------------------------------------
class HueSaturationAdjustment : public Adjustment {
public:
    explicit HueSaturationAdjustment(const HueSaturationParams& p) : Adjustment(p), k_(hue_sat_scalars(p)) {}
    const char* type() const override { return "hue_saturation"; }
    void apply(uint8_t& r, uint8_t& g, uint8_t& b) const override { hue_sat_pixel(k_, r, g, b); }

private:
    HueSatScalars k_;
};

class BlackWhiteAdjustment : public Adjustment {
public:
    explicit BlackWhiteAdjustment(const BlackWhiteParams& p) : Adjustment(p), k_(black_white_scalars(p)) {}
    const char* type() const override { return "black_white"; }
    void apply(uint8_t& r, uint8_t& g, uint8_t& b) const override { black_white_pixel(k_, r, g, b); }

private:
    BlackWhiteScalars k_;
};

class ThresholdAdjustment : public Adjustment {
public:
    explicit ThresholdAdjustment(const ThresholdParams& p) : Adjustment(p), level_(p.level) {}
    const char* type() const override { return "threshold"; }
    void apply(uint8_t& r, uint8_t& g, uint8_t& b) const override { r = g = b = threshold_value(level_, r, g, b); }

private:
    int level_;
};

struct Builder {
    std::shared_ptr<const Adjustment> operator()(const LevelsParams& p) const {
        return std::make_shared<LutAdjustment>(p, "levels", levels_lut(p.r, p.rgb), levels_lut(p.g, p.rgb),
                                               levels_lut(p.b, p.rgb));
    }
    std::shared_ptr<const Adjustment> operator()(const CurvesParams& p) const {
        const CurveSpline rgb(p.rgb), r(p.r), g(p.g), b(p.b);
        return std::make_shared<LutAdjustment>(p, "curves", curves_lut(r, rgb), curves_lut(g, rgb), curves_lut(b, rgb));
    }
    std::shared_ptr<const Adjustment> operator()(const BrightnessContrastParams& p) const {
        const Lut8 lut = brightness_contrast_lut(p);
        return std::make_shared<LutAdjustment>(p, "brightness_contrast", lut, lut, lut);
    }
    std::shared_ptr<const Adjustment> operator()(const HueSaturationParams& p) const {
        return std::make_shared<HueSaturationAdjustment>(p);
    }
    std::shared_ptr<const Adjustment> operator()(const BlackWhiteParams& p) const {
        return std::make_shared<BlackWhiteAdjustment>(p);
    }
    std::shared_ptr<const Adjustment> operator()(const InvertParams& p) const {
        const Lut8 lut = invert_lut();
        return std::make_shared<LutAdjustment>(p, "invert", lut, lut, lut);
    }
    std::shared_ptr<const Adjustment> operator()(const PosterizeParams& p) const {
        const Lut8 lut = posterize_lut(p.levels);
        return std::make_shared<LutAdjustment>(p, "posterize", lut, lut, lut);
    }
    std::shared_ptr<const Adjustment> operator()(const ThresholdParams& p) const {
        return std::make_shared<ThresholdAdjustment>(p);
    }
};

// ---- render-script parsing ---------------------------------------------------------------------------
const Json* sub_object(Fields& f, const char* key) {
    const Json* v = f.raw(key);
    if (v && !v->is_object()) f.fail(std::string("field '") + key + "' must be an object");
    return v;
}

LevelsSetting parse_setting(const Json& obj, const std::string& ctx) {
    Fields f(obj, ctx);
    LevelsSetting s;
    s.in_black = static_cast<int>(f.int_or("in_black", 0, 0, 254));
    s.in_white = static_cast<int>(f.int_or("in_white", 255, 1, 255));
    s.gamma = f.double_or("gamma", 1.0, 0.1, 9.99);
    s.out_black = static_cast<int>(f.int_or("out_black", 0, 0, 255));
    s.out_white = static_cast<int>(f.int_or("out_white", 255, 0, 255));
    f.finish();
    if (!(s.in_black < s.in_white)) f.fail("in_black must be below in_white");
    return s;
}

LevelsParams parse_levels(Fields& f) {
    LevelsParams p;
    const char* keys[] = {"rgb", "r", "g", "b"};
    LevelsSetting* dst[] = {&p.rgb, &p.r, &p.g, &p.b};
    for (int i = 0; i < 4; ++i)
        if (const Json* v = sub_object(f, keys[i])) *dst[i] = parse_setting(*v, f.context() + "." + keys[i]);
    f.finish();
    return p;
}

CurvePoints parse_curve(const Json& v, const std::string& ctx) {
    if (!v.is_array()) Fields::fail_at(ctx, "a curve must be an array of [in, out] points");
    if (v.size() < 2 || v.size() > 16)
        Fields::fail_at(ctx, "a curve needs 2..16 points, got " + std::to_string(v.size()));
    CurvePoints pts;
    for (size_t i = 0; i < v.size(); ++i) {
        const Json& pt = v[i];
        const std::string what = ctx + "[" + std::to_string(i) + "]";
        if (!pt.is_array() || pt.size() != 2) Fields::fail_at(what, "a point must be [in, out]");
        const int x = static_cast<int>(Fields::as_int(pt[0], what + "[0]", 0, 255));
        const int y = static_cast<int>(Fields::as_int(pt[1], what + "[1]", 0, 255));
        if (!pts.empty() && !(x > pts.back()[0])) Fields::fail_at(what, "point inputs must be strictly increasing");
        pts.push_back({{x, y}});
    }
    return pts;
}

CurvesParams parse_curves(Fields& f) {
    CurvesParams p;
    const char* keys[] = {"rgb", "r", "g", "b"};
    CurvePoints* dst[] = {&p.rgb, &p.r, &p.g, &p.b};
    for (int i = 0; i < 4; ++i)
        if (const Json* v = f.raw(keys[i])) *dst[i] = parse_curve(*v, f.context() + "." + keys[i]);
    f.finish();
    return p;
}

BrightnessContrastParams parse_bc(Fields& f) {
    BrightnessContrastParams p;
    p.brightness = f.double_or("brightness", 0.0, -1.0, 1.0);
    p.contrast = f.double_or("contrast", 0.0, -1.0, 1.0);
    f.finish();
    return p;
}

HueSaturationParams parse_hs(Fields& f) {
    HueSaturationParams p;
    p.colorize = f.bool_or("colorize", false);
    if (p.colorize) {
        p.hue = f.double_or("hue", 0.0, 0.0, 360.0);
        p.saturation = f.double_or("saturation", 25.0, 0.0, 100.0);
    } else {
        p.hue = f.double_or("hue", 0.0, -180.0, 180.0);
        p.saturation = f.double_or("saturation", 0.0, -100.0, 100.0);
    }
    p.lightness = f.double_or("lightness", 0.0, -100.0, 100.0);
    f.finish();
    return p;
}

BlackWhiteParams parse_bw(Fields& f) {
    BlackWhiteParams p;
    p.reds = f.double_or("reds", 40.0, -200.0, 300.0);
    p.yellows = f.double_or("yellows", 60.0, -200.0, 300.0);
    p.greens = f.double_or("greens", 40.0, -200.0, 300.0);
    p.cyans = f.double_or("cyans", 60.0, -200.0, 300.0);
    p.blues = f.double_or("blues", 20.0, -200.0, 300.0);
    p.magentas = f.double_or("magentas", 80.0, -200.0, 300.0);
    if (const Json* t = f.raw("tint"); t && !t->is_null()) {
        if (!t->is_string()) f.fail("field 'tint' must be a \"#RRGGBB\" string or null");
        const std::string s = t->get<std::string>();
        const auto c = parse_hex_color(s);
        if (s.size() != 7 || !c) f.fail("field 'tint' is not a colour \"#RRGGBB\": '" + s + "'");
        p.tint = *c;
    }
    f.finish();
    return p;
}

std::string hex_rgb(Rgba8 c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

Json setting_json(const LevelsSetting& s) {
    return Json{{"in_black", s.in_black},   {"in_white", s.in_white},   {"gamma", s.gamma},
                {"out_black", s.out_black}, {"out_white", s.out_white}};
}

Json curve_json(const CurvePoints& c) {
    Json a = Json::array();
    for (const auto& p : c) a.push_back(Json::array({p[0], p[1]}));
    return a;
}

struct ToJson {
    Json operator()(const LevelsParams& p) const {
        return Json{{"rgb", setting_json(p.rgb)}, {"r", setting_json(p.r)}, {"g", setting_json(p.g)}, {"b", setting_json(p.b)}};
    }
    Json operator()(const CurvesParams& p) const {
        return Json{{"rgb", curve_json(p.rgb)}, {"r", curve_json(p.r)}, {"g", curve_json(p.g)}, {"b", curve_json(p.b)}};
    }
    Json operator()(const BrightnessContrastParams& p) const {
        return Json{{"brightness", p.brightness}, {"contrast", p.contrast}};
    }
    Json operator()(const HueSaturationParams& p) const {
        return Json{{"colorize", p.colorize}, {"hue", p.hue}, {"saturation", p.saturation}, {"lightness", p.lightness}};
    }
    Json operator()(const BlackWhiteParams& p) const {
        Json j{{"reds", p.reds},   {"yellows", p.yellows}, {"greens", p.greens},
               {"cyans", p.cyans}, {"blues", p.blues},     {"magentas", p.magentas}};
        j["tint"] = p.tint ? Json(hex_rgb(*p.tint)) : Json(nullptr);
        return j;
    }
    Json operator()(const InvertParams&) const { return Json::object(); }
    Json operator()(const PosterizeParams& p) const { return Json{{"levels", p.levels}}; }
    Json operator()(const ThresholdParams& p) const { return Json{{"level", p.level}}; }
};

}  // namespace

bool is_known_type(const std::string& type) {
    for (const char* t : kKnownTypes)
        if (type == t) return true;
    return false;
}

const char* type_name(const AdjustmentParams& p) { return kKnownTypes[p.index()]; }

std::shared_ptr<const Adjustment> make_adjustment(const AdjustmentParams& params, const std::string& context) {
    std::visit(Validator{context}, params);
    return std::visit(Builder{}, params);
}

AdjustmentParams parse_adjustment_params(const std::string& type, const nlohmann::json* params,
                                         const std::string& context) {
    if (!is_known_type(type)) throw ScriptError(context + ": unknown adjustment type '" + type + "'");
    static const Json kEmpty = Json::object();
    const Json& p = params ? *params : kEmpty;
    if (!p.is_object()) throw ScriptError(context + ": field 'params' must be an object");
    Fields f(p, context + " params");
    if (type == "levels") return parse_levels(f);
    if (type == "curves") return parse_curves(f);
    if (type == "brightness_contrast") return parse_bc(f);
    if (type == "hue_saturation") return parse_hs(f);
    if (type == "black_white") return parse_bw(f);
    if (type == "posterize") {
        PosterizeParams q;
        q.levels = static_cast<int>(f.int_or("levels", 4, 2, 255));
        f.finish();
        return q;
    }
    if (type == "threshold") {
        ThresholdParams q;
        q.level = static_cast<int>(f.int_or("level", 128, 1, 255));
        f.finish();
        return q;
    }
    f.finish();  // invert: {} only
    return InvertParams{};
}

std::shared_ptr<const Adjustment> make_adjustment(const std::string& type, const nlohmann::json* params,
                                                  const std::string& context) {
    return make_adjustment(parse_adjustment_params(type, params, context), context);
}

nlohmann::json params_to_json(const AdjustmentParams& p) { return std::visit(ToJson{}, p); }

}  // namespace rl::adjust
