#include "nix/parsed-derivations.hh"

#include <nlohmann/json.hpp>
#include <regex>

namespace nix {

ParsedDerivation::ParsedDerivation(const StorePath & drvPath, BasicDerivation & drv)
    : drvPath(drvPath), drv(drv)
{
    /* Parse the __json attribute, if any. */
    auto jsonAttr = drv.env.find("__json");
    if (jsonAttr != drv.env.end()) {
        try {
            structuredAttrs = std::make_unique<nlohmann::json>(nlohmann::json::parse(jsonAttr->second));
        } catch (std::exception & e) {
            throw Error("cannot process __json attribute of '%s': %s", drvPath.to_string(), e.what());
        }
    }
}

ParsedDerivation::~ParsedDerivation() { }

std::optional<std::string> ParsedDerivation::getStringAttr(const std::string & name) const
{
    if (structuredAttrs) {
        auto i = structuredAttrs->find(name);
        if (i == structuredAttrs->end())
            return {};
        else {
            if (!i->is_string())
                throw Error("attribute '%s' of derivation '%s' must be a string", name, drvPath.to_string());
            return i->get<std::string>();
        }
    } else {
        auto i = drv.env.find(name);
        if (i == drv.env.end())
            return {};
        else
            return i->second;
    }
}

bool ParsedDerivation::getBoolAttr(const std::string & name, bool def) const
{
    if (structuredAttrs) {
        auto i = structuredAttrs->find(name);
        if (i == structuredAttrs->end())
            return def;
        else {
            if (!i->is_boolean())
                throw Error("attribute '%s' of derivation '%s' must be a Boolean", name, drvPath.to_string());
            return i->get<bool>();
        }
    } else {
        auto i = drv.env.find(name);
        if (i == drv.env.end())
            return def;
        else
            return i->second == "1";
    }
}

std::optional<Strings> ParsedDerivation::getStringsAttr(const std::string & name) const
{
    if (structuredAttrs) {
        auto i = structuredAttrs->find(name);
        if (i == structuredAttrs->end())
            return {};
        else {
            if (!i->is_array())
                throw Error("attribute '%s' of derivation '%s' must be a list of strings", name, drvPath.to_string());
            Strings res;
            for (auto j = i->begin(); j != i->end(); ++j) {
                if (!j->is_string())
                    throw Error("attribute '%s' of derivation '%s' must be a list of strings", name, drvPath.to_string());
                res.push_back(j->get<std::string>());
            }
            return res;
        }
    } else {
        auto i = drv.env.find(name);
        if (i == drv.env.end())
            return {};
        else
            return tokenizeString<Strings>(i->second);
    }
}

std::optional<StringSet> ParsedDerivation::getStringSetAttr(const std::string & name) const
{
    auto ss = getStringsAttr(name);
    return ss
        ? (std::optional{StringSet{ss->begin(), ss->end()}})
        : (std::optional<StringSet>{});
}

static std::regex shVarName("[A-Za-z_][A-Za-z0-9_]*");

/**
 * Write a JSON representation of store object metadata, such as the
 * hash and the references.
 *
 * @note Do *not* use `ValidPathInfo::toJSON` because this function is
 * subject to stronger stability requirements since it is used to
 * prepare build environments. Perhaps someday we'll have a versionining
 * mechanism to allow this to evolve again and get back in sync, but for
 * now we must not change - not even extend - the behavior.
 */
static nlohmann::json pathInfoToJSON(
    Store & store,
    const StorePathSet & storePaths)
{
    using nlohmann::json;

    nlohmann::json::array_t jsonList = json::array();

    for (auto & storePath : storePaths) {
        auto info = store.queryPathInfo(storePath);

        auto & jsonPath = jsonList.emplace_back(json::object());

        jsonPath["narHash"] = info->narHash.to_string(HashFormat::Nix32, true);
        jsonPath["narSize"] = info->narSize;

        {
            auto & jsonRefs = jsonPath["references"] = json::array();
            for (auto & ref : info->references)
                jsonRefs.emplace_back(store.printStorePath(ref));
        }

        if (info->ca)
            jsonPath["ca"] = renderContentAddress(info->ca);

        // Add the path to the object whose metadata we are including.
        jsonPath["path"] = store.printStorePath(storePath);

        jsonPath["valid"] = true;

        jsonPath["closureSize"] = ({
            uint64_t totalNarSize = 0;
            StorePathSet closure;
            store.computeFSClosure(info->path, closure, false, false);
            for (auto & p : closure) {
                auto info = store.queryPathInfo(p);
                totalNarSize += info->narSize;
            }
            totalNarSize;
        });
    }
    return jsonList;
}

std::optional<nlohmann::json> ParsedDerivation::prepareStructuredAttrs(Store & store, const StorePathSet & inputPaths)
{
    if (!structuredAttrs) return std::nullopt;

    auto json = *structuredAttrs;

    /* Add an "outputs" object containing the output paths. */
    nlohmann::json outputs;
    for (auto & i : drv.outputs)
        outputs[i.first] = hashPlaceholder(i.first);
    json["outputs"] = outputs;

    /* Handle exportReferencesGraph. */
    auto e = json.find("exportReferencesGraph");
    if (e != json.end() && e->is_object()) {
        for (auto i = e->begin(); i != e->end(); ++i) {
            StorePathSet storePaths;
            for (auto & p : *i)
                storePaths.insert(store.toStorePath(p.get<std::string>()).first);
            json[i.key()] = pathInfoToJSON(store,
                store.exportReferences(storePaths, inputPaths));
        }
    }

    return json;
}

/* As a convenience to bash scripts, write a shell file that
   maps all attributes that are representable in bash -
   namely, strings, integers, nulls, Booleans, and arrays and
   objects consisting entirely of those values. (So nested
   arrays or objects are not supported.) */
std::string writeStructuredAttrsShell(const nlohmann::json & json)
{

    auto handleSimpleType = [](const nlohmann::json & value) -> std::optional<std::string> {
        if (value.is_string())
            return shellEscape(value.get<std::string_view>());

        if (value.is_number()) {
            auto f = value.get<float>();
            if (std::ceil(f) == f)
                return std::to_string(value.get<int>());
        }

        if (value.is_null())
            return std::string("''");

        if (value.is_boolean())
            return value.get<bool>() ? std::string("1") : std::string("");

        return {};
    };

    std::string jsonSh;

    for (auto & [key, value] : json.items()) {

        if (!std::regex_match(key, shVarName)) continue;

        auto s = handleSimpleType(value);
        if (s)
            jsonSh += fmt("declare %s=%s\n", key, *s);

        else if (value.is_array()) {
            std::string s2;
            bool good = true;

            for (auto & value2 : value) {
                auto s3 = handleSimpleType(value2);
                if (!s3) { good = false; break; }
                s2 += *s3; s2 += ' ';
            }

            if (good)
                jsonSh += fmt("declare -a %s=(%s)\n", key, s2);
        }

        else if (value.is_object()) {
            std::string s2;
            bool good = true;

            for (auto & [key2, value2] : value.items()) {
                auto s3 = handleSimpleType(value2);
                if (!s3) { good = false; break; }
                s2 += fmt("[%s]=%s ", shellEscape(key2), *s3);
            }

            if (good)
                jsonSh += fmt("declare -A %s=(%s)\n", key, s2);
        }
    }

    return jsonSh;
}
}
