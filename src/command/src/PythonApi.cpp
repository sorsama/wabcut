#include <wabcut/command/PythonApi.h>

#include <wabcut/command/Schema.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string_view>

namespace wabcut::command {

namespace {

const std::set<std::string, std::less<>>& reservedWords() {
    static const std::set<std::string, std::less<>> words{
        "False",  "None",   "True",    "and",    "as",       "assert", "async", "await",  "break",
        "class",  "continue", "def",   "del",    "elif",     "else",   "except", "finally", "for",
        "from",   "global", "if",      "import", "in",       "is",     "lambda", "nonlocal", "not",
        "or",     "pass",   "raise",   "return", "try",      "while",  "with",  "yield",  "self"};
    return words;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_')) {
        return false;
    }
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

/// "export.queue" -> "ExportQueue", "textStyle" -> "TextStyle", "color.r" -> "ColorR".
std::string pascal(std::string_view text) {
    std::string out;
    bool upper = true;
    for (const char c : text) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            upper = true;
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    return out;
}

std::string pyString(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char* hex = "0123456789abcdef";
                out += "\\x";
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string pyLiteral(const Json& v) {
    if (v.is_string()) {
        return pyString(v.get<std::string>());
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "True" : "False";
    }
    if (v.is_null()) {
        return "None";
    }
    return v.dump();
}

/// Text for a docstring body: backslashes and triple quotes escaped, every line indented.
std::string docText(const std::string& text, const std::string& indent) {
    std::string escaped;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\') {
            escaped += "\\\\";
        } else if (c == '"' && text.compare(i, 3, "\"\"\"") == 0) {
            escaped += "\\\"\\\"\\\"";
            i += 2;
        } else if (c == '\r') {
            continue;
        } else {
            escaped += c;
        }
    }
    std::string out;
    std::size_t start = 0;
    while (start <= escaped.size()) {
        const std::size_t nl = escaped.find('\n', start);
        const std::string line = escaped.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        // Trailing spaces are noise; a line that is only a quote must not end the docstring early.
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
            trimmed.pop_back();
        }
        out += trimmed.empty() ? std::string() : indent + trimmed;
        if (nl == std::string::npos) {
            break;
        }
        out += '\n';
        start = nl + 1;
    }
    return out;
}

struct Names {
    std::vector<std::pair<std::string, std::string>> typedDicts; ///< name -> definition line, in first-use order
    std::set<std::string> used;

    std::string reserve(const std::string& wanted) {
        std::string name = wanted;
        for (int n = 2; used.contains(name); ++n) {
            name = wanted + std::to_string(n);
        }
        used.insert(name);
        return name;
    }
};

std::string typeFor(const Json& schema, const std::string& hint, Names& names);

std::string typedDict(const Json& schema, const std::string& hint, Names& names) {
    const std::string name = names.reserve(hint);
    std::set<std::string> required;
    if (schema.contains("required") && schema["required"].is_array()) {
        for (const Json& r : schema["required"]) {
            required.insert(r.get<std::string>());
        }
    }
    // Reserve the slot first so the definitions come out in the order the types are met (nested ones after).
    const std::size_t slot = names.typedDicts.size();
    names.typedDicts.emplace_back(name, std::string());
    std::string body;
    for (const auto& [key, prop] : schema["properties"].items()) {
        const std::string type = typeFor(prop, name + "_" + pascal(key), names);
        body += (body.empty() ? "" : ", ") + pyString(key) + ": " + (required.contains(key) ? "Required[" : "NotRequired[") +
                type + "]";
    }
    names.typedDicts[slot].second = name + " = TypedDict(" + pyString(name) + ", {" + body + "})";
    return name;
}

std::string typeFor(const Json& schema, const std::string& hint, Names& names) {
    if (!schema.is_object()) {
        return "Any";
    }
    if (schema.contains("const")) {
        return "Literal[" + pyLiteral(schema["const"]) + "]";
    }
    std::vector<std::string> types;
    const auto addType = [&](std::string t) {
        if (std::find(types.begin(), types.end(), t) == types.end()) {
            types.push_back(std::move(t));
        }
    };
    bool allowsNull = false;
    std::vector<std::string> typeNames;
    if (schema.contains("type")) {
        if (schema["type"].is_string()) {
            typeNames.push_back(schema["type"].get<std::string>());
        } else if (schema["type"].is_array()) {
            for (const Json& t : schema["type"]) {
                typeNames.push_back(t.get<std::string>());
            }
        }
    }
    allowsNull = std::find(typeNames.begin(), typeNames.end(), "null") != typeNames.end();
    if (schema.contains("enum") && schema["enum"].is_array() && !schema["enum"].empty()) {
        std::string literal;
        bool hasNull = false;
        for (const Json& v : schema["enum"]) {
            hasNull = hasNull || v.is_null();
            literal += (literal.empty() ? "" : ", ") + pyLiteral(v);
        }
        std::string out = "Literal[" + literal + "]";
        if (allowsNull && !hasNull) {
            out += " | None";
        }
        return out;
    }
    if (typeNames.empty()) {
        return "Any";
    }
    for (const std::string& t : typeNames) {
        if (t == "integer") {
            addType("int");
        } else if (t == "number") {
            addType("float");
        } else if (t == "string") {
            addType("str");
        } else if (t == "boolean") {
            addType("bool");
        } else if (t == "array") {
            addType("list[" + (schema.contains("items") ? typeFor(schema["items"], hint + "Item", names) : std::string("Any")) +
                    "]");
        } else if (t == "object") {
            if (schema.contains("properties") && schema["properties"].is_object() && !schema["properties"].empty()) {
                addType(typedDict(schema, hint, names));
            } else {
                addType("dict[str, Any]");
            }
        }
    }
    std::string out;
    for (const std::string& t : types) {
        out += (out.empty() ? "" : " | ") + t;
    }
    if (allowsNull) {
        out += out.empty() ? "None" : " | None";
    }
    return out.empty() ? "Any" : out;
}

// --- Groups of commands (session.clip, session.export.queue) ----------------------------------------------------

struct Node {
    const CommandSpec* command = nullptr;
    std::map<std::string, Node> children;
};

std::string method(const CommandSpec& spec, const std::string& methodName, Names& names, bool callable) {
    const std::string indent = "    ";
    const Json& params = spec.params;
    std::vector<std::string> required;
    if (params.contains("required") && params["required"].is_array()) {
        for (const Json& r : params["required"]) {
            required.push_back(r.get<std::string>());
        }
    }
    struct Param {
        std::string wire, py, type, doc;
        bool required = false;
    };
    std::vector<Param> list;
    if (params.contains("properties") && params["properties"].is_object()) {
        for (const auto& [key, prop] : params["properties"].items()) {
            if (!isIdentifier(key)) {
                continue; // cannot be spelled as a keyword: pass it through the dict form
            }
            Param p;
            p.wire = key;
            p.py = reservedWords().contains(key) ? key + "_" : key;
            p.required = std::find(required.begin(), required.end(), key) != required.end();
            p.type = typeFor(prop, pascal(spec.name) + "_" + pascal(key), names);
            p.doc = prop.is_object() ? prop.value("description", std::string()) : std::string();
            if (prop.is_object() && prop.contains("default")) {
                p.doc += std::string(p.doc.empty() ? "" : " ") + "(Default: " + pyLiteral(prop["default"]) + ".)";
            }
            list.push_back(std::move(p));
        }
    }
    std::stable_sort(list.begin(), list.end(), [](const Param& a, const Param& b) { return a.required && !b.required; });

    const bool edit = spec.mutates();
    std::string out = indent + "def " + (callable ? "__call__" : methodName) + "(\n" + indent + "    self,\n" + indent +
                      "    _params: dict[str, Any] | None = ...,\n" + indent + "    /,\n";
    if (!list.empty() || edit) {
        out += indent + "    *,\n"; // a bare * must be followed by a keyword parameter
    }
    for (const Param& p : list) {
        out += indent + "    " + p.py + ": " + p.type + (p.required ? "" : " = ...") + ",\n";
    }
    if (edit) {
        const std::set<std::string> taken = [&] {
            std::set<std::string> s;
            for (const Param& p : list) {
                s.insert(p.wire);
            }
            return s;
        }();
        for (const auto& [name, type] : std::vector<std::pair<std::string, std::string>>{
                 {"dryRun", "bool"}, {"expectedRevision", "int"}, {"idempotencyKey", "str"}}) {
            if (!taken.contains(name)) {
                out += indent + "    " + name + ": " + type + " = ...,\n";
            }
        }
    }
    out += indent + ") -> " + (edit ? "Result" : "Any") + ":\n";

    // The docstring: title, description, how to call it, the parameters.
    const std::string body = indent + "    ";
    std::string doc = body + "\"\"\"" + docText(spec.title, "").substr(0) + "\n\n";
    doc += docText(spec.description, body) + "\n\n";
    std::string facts = body + "Command ``" + spec.name + "`` (needs " + std::string(toString(spec.capability));
    if (spec.runsModel) {
        facts += "; runs an AI model, so MediaAnalysis is needed too";
    }
    if (spec.touchesFiles) {
        facts += "; reads or writes files its parameters name";
    }
    facts += ").";
    if (!spec.aliases.empty()) {
        facts += " Also known as";
        for (std::size_t i = 0; i < spec.aliases.size(); ++i) {
            facts += (i == 0 ? " " : ", ") + std::string("``") + spec.aliases[i] + "``";
        }
        facts += ".";
    }
    doc += facts + "\n";
    doc += body + (edit ? "Returns the Result of the edit (``.data`` holds the command's own answer)."
                        : "Returns the command's answer (the data of the read).") +
           "\n";
    if (!list.empty() || edit) {
        doc += "\n" + body + "Args:\n";
        doc += body + "    _params: The parameters as one dict instead of keywords (a keyword wins); for a name that\n" +
               body + "        cannot be a keyword.\n";
        for (const Param& p : list) {
            std::string text = docText(p.doc.empty() ? std::string("(no description)") : p.doc, "");
            std::string wrapped;
            for (const char c : text) {
                wrapped += c;
                if (c == '\n') {
                    wrapped += body + "        ";
                }
            }
            doc += body + "    " + p.py + ": " + wrapped + (p.required ? " (Required.)" : "") + "\n";
        }
        if (edit) {
            doc += body + "    dryRun: Preview the edit without committing it.\n";
            doc += body + "    expectedRevision: Fail with StaleRevision unless the project is at this revision.\n";
            doc += body + "    idempotencyKey: A repeated call with the same key returns the first result.\n";
        }
    }
    doc += body + "\"\"\"\n";
    return out + doc;
}

std::string groupClassName(const std::string& path) { return pascal(path) + "Commands"; }

void emitNode(const std::string& path, const Node& node, Names& names, std::string& out) {
    // Children first so a class is defined before the group that holds it (stubs allow forward references, but
    // readers do not).
    for (const auto& [name, child] : node.children) {
        if (!child.children.empty()) {
            emitNode(path.empty() ? name : path + "." + name, child, names, out);
        }
    }
    if (path.empty()) {
        return;
    }
    out += "class " + groupClassName(path) + ":\n";
    if (node.command) {
        out += method(*node.command, node.command->name, names, true) + "\n";
    }
    for (const auto& [name, child] : node.children) {
        if (!child.children.empty()) {
            out += "    " + (reservedWords().contains(name) ? name + "_" : name) + ": " +
                   groupClassName(path + "." + name) + "\n";
        }
    }
    for (const auto& [name, child] : node.children) {
        if (child.command && child.children.empty()) {
            out += "\n" + method(*child.command, reservedWords().contains(name) ? name + "_" : name, names, false);
        }
    }
    out += "\n\n";
}

std::string catalogJson(const std::vector<const CommandSpec*>& specs) {
    std::string out = "{\"format\": 1, \"commands\": [\n";
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const CommandSpec& spec = *specs[i];
        Json entry{{"name", spec.name},
                   {"version", spec.version},
                   {"title", spec.title},
                   {"capability", std::string(toString(spec.capability))},
                   {"mutates", spec.mutates()},
                   {"aliases", spec.aliases},
                   {"description", spec.description},
                   {"runsModel", spec.runsModel},
                   {"touchesFiles", spec.touchesFiles},
                   {"fileParams", spec.fileParams},
                   {"params", spec.params}};
        if (!spec.example.is_null()) {
            entry["example"] = spec.example;
        }
        out += "  " + entry.dump() + (i + 1 < specs.size() ? ",\n" : "\n");
    }
    return out + "]}\n";
}

std::string stub(const std::vector<const CommandSpec*>& specs) {
    Names names;
    Node root;
    for (const CommandSpec* spec : specs) {
        Node* node = &root;
        std::size_t start = 0;
        while (start <= spec->name.size()) {
            const std::size_t dot = spec->name.find('.', start);
            const std::string part = spec->name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            node = &node->children[part];
            if (dot == std::string::npos) {
                break;
            }
            start = dot + 1;
        }
        node->command = spec;
    }
    // A command with no dot has no group: it would be `session.<name>`; the catalog has none, but keep it valid.
    std::string classes;
    emitNode(std::string(), root, names, classes);

    std::string out =
        "# Generated by wabcut_command_docgen (src/command/src/PythonApi.cpp); do not edit by hand.\n"
        "# Regenerate: build\\win-msvc\\bin\\wabcut_command_docgen.exe docs/commands.md docs/mcp.md docs/extensions.md "
        "--python python/wabcut\n"
        "\"\"\"Typed stubs for the command catalog: every command as `session.<group>.<command>(...)`.\n\n"
        "The names, parameters and descriptions are the catalog's (docs/commands.md). Edits return a Result; reads return\n"
        "the command's answer. Parameter names that are Python keywords carry a trailing underscore (`from_`).\n"
        "\"\"\"\n\n"
        "from typing import Any, Literal, NotRequired, Required, TypedDict\n\n"
        "from ._result import Result\n\n\n";
    for (const auto& [name, definition] : names.typedDicts) {
        out += definition + "\n";
    }
    out += "\n\n" + classes;
    out += "class Commands:\n"
           "    \"\"\"The commands of a session, grouped as in their names: `session.clip.addEffect(...)`.\"\"\"\n\n";
    for (const auto& [name, child] : root.children) {
        if (!child.children.empty()) {
            out += "    " + (reservedWords().contains(name) ? name + "_" : name) + ": " + groupClassName(name) + "\n";
        }
    }
    return out;
}

} // namespace

bool isPythonReserved(const std::string& name) { return reservedWords().contains(name); }

std::vector<PythonFile> pythonApiFiles(const CommandRegistry& registry) {
    std::vector<const CommandSpec*> newest;
    const auto specs = registry.all();
    for (std::size_t i = 0; i < specs.size(); ++i) {
        // all() is sorted by name then version: the last entry of a name is its newest version.
        if (i + 1 < specs.size() && specs[i + 1].name == specs[i].name) {
            continue;
        }
        newest.push_back(&specs[i]);
    }
    return {{"catalog.json", catalogJson(newest)}, {"_commands.pyi", stub(newest)}};
}

} // namespace wabcut::command
