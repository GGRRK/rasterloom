// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/script/engine.hpp"

#include <fstream>
#include <sstream>

#include "core/base/error.hpp"
#include "core/script/domains.hpp"

namespace rl::script {

const OpRegistry& default_registry() {
    static const OpRegistry reg = [] {
        OpRegistry r;
        registerAllOps(r);
        return r;
    }();
    return reg;
}

ScriptResult run_script(const Json& script, const OpRegistry& registry) {
    Fields top(script, "script");
    const Json& canvas = top.raw_req("canvas");
    const Json& ops = top.raw_req("ops");
    const std::string out = top.req_enum("out", {"png8"});
    top.finish();

    Fields cf(canvas, "canvas");
    const int w = static_cast<int>(cf.req_int("w", 1, kMaxCanvasSide));
    const int h = static_cast<int>(cf.req_int("h", 1, kMaxCanvasSide));
    const Rgba8 bg = cf.color_or("bg", Rgba8{0, 0, 0, 0});
    cf.finish();

    if (!ops.is_array()) throw ScriptError("script: field 'ops' must be an array");

    ScriptResult res;
    res.out = out;
    res.doc = std::make_unique<Document>(w, h, bg, Document::kScriptHistoryDepth);
    Document& doc = *res.doc;

    for (size_t i = 0; i < ops.size(); ++i) {
        const Json& op = ops[i];
        const std::string where = "op #" + std::to_string(i);
        if (!op.is_object()) throw ScriptError(where + ": must be a JSON object");
        auto name_it = op.find("op");
        if (name_it == op.end() || !name_it->is_string())
            throw ScriptError(where + ": missing string field 'op'");
        const std::string name = name_it->get<std::string>();
        const OpSpec* spec = registry.find(name);
        if (!spec) throw ScriptError(where + ": unknown op '" + name + "'");

        OpContext ctx{doc, i, where + " (" + name + ")"};
        Fields f(op, ctx.label);
        f.consume("op");
        if (spec->records_history) doc.push_history();
        spec->run(ctx, f);
        f.finish();  // backstop: a handler that forgot finish() still rejects unknown fields
    }
    return res;
}

ScriptResult run_script_text(const std::string& text, const OpRegistry& registry) {
    Json j;
    try {
        j = Json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        throw ScriptError(std::string("invalid JSON: ") + e.what());
    }
    return run_script(j, registry);
}

ScriptResult run_script_file(const std::string& path, const OpRegistry& registry) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ScriptError("cannot read script file '" + path + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    return run_script_text(ss.str(), registry);
}

}  // namespace rl::script
