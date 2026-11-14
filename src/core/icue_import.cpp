// Reads iCUE 5's cereal XML files: cooling/configurations (curves), sensors/UserProps (names),
// profiles/tree.cueprofileorder + profiles/{id}.cueprofiledata (which curve drives which fan).
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include "core/model.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// Minimal XML: elements, text, entities. Attributes are skipped.
struct Node {
    std::string tag, text;
    std::vector<std::unique_ptr<Node>> children;
    const Node* child(const char* name) const {
        for (auto& c : children)
            if (c->tag == name) return c.get();
        return nullptr;
    }
    std::string value(const char* name) const {
        const Node* c = child(name);
        return c ? c->text : std::string();
    }
};

std::string decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        std::string e = semi == std::string::npos ? "" : s.substr(i + 1, semi - i - 1);
        if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "amp") out += '&';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (!e.empty() && e[0] == '#') out += (char)strtol(e.c_str() + (e[1] == 'x' ? 2 : 1), nullptr, e[1] == 'x' ? 16 : 10);
        else {
            out += '&';
            continue;
        }
        i = semi;
    }
    return out;
}

std::unique_ptr<Node> parseXml(const std::string& xml) {
    auto root = std::make_unique<Node>();
    std::vector<Node*> stack = {root.get()};
    size_t i = 0;
    while (i < xml.size()) {
        size_t lt = xml.find('<', i);
        if (lt == std::string::npos) break;
        if (lt > i) stack.back()->text += decode(xml.substr(i, lt - i));
        size_t gt = xml.find('>', lt);
        if (gt == std::string::npos) break;
        std::string tag = xml.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
        if (tag[0] == '/') {
            if (stack.size() > 1) stack.pop_back();
            continue;
        }
        bool selfClosing = tag.back() == '/';
        if (selfClosing) tag.pop_back();
        size_t space = tag.find_first_of(" \t\r\n");
        auto node = std::make_unique<Node>();
        node->tag = space == std::string::npos ? tag : tag.substr(0, space);
        Node* raw = node.get();
        stack.back()->children.push_back(std::move(node));
        if (!selfClosing) stack.push_back(raw);
    }
    // Text of elements with children is whitespace noise; keep leaf text trimmed.
    struct Trim {
        static void run(Node* n) {
            size_t a = n->text.find_first_not_of(" \t\r\n"), b = n->text.find_last_not_of(" \t\r\n");
            n->text = a == std::string::npos ? std::string() : n->text.substr(a, b - a + 1);
            for (auto& c : n->children) run(c.get());
        }
    };
    Trim::run(root.get());
    return root;
}

std::unique_ptr<Node> loadXml(const std::string& path) {
#ifdef _WIN32
    wchar_t wide[1024];
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide, 1024);
    FILE* f = _wfopen(wide, L"rb");
#else
    FILE* f = fopen(path.c_str(), "rb");
#endif
    if (!f) return nullptr;
    std::string data;
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) data.append(buffer, n);
    fclose(f);
    return parseXml(data);
}

template <typename Fn>
void walk(const Node* n, Fn fn) {
    fn(n);
    for (auto& c : n->children) walk(c.get(), fn);
}

// "vid<1b1c>pid<000c10>serial<..>index<0>senstype<Temperature>sensindex<1>" -> field("senstype") = "Temperature"
std::string field(const std::string& id, const char* name) {
    std::string key = std::string(name) + "<";
    size_t p = id.find(key);
    if (p == std::string::npos) return std::string();
    p += key.size();
    // serial<> can hold a device path; take the field up to the matching '>'.
    int depth = 1;
    size_t q = p;
    for (; q < id.size() && depth; q++) {
        if (id[q] == '<') depth++;
        if (id[q] == '>') depth--;
    }
    return id.substr(p, q - p - 1);
}

// iCUE sensor/channel id -> our id ("" if not ours).
std::string mapSensor(const std::string& id) {
    std::string pid = field(id, "pid"), type = field(id, "senstype"), index = field(id, "sensindex");
    if (pid == "000c10" && type == "Temperature") return "cp.t" + index;
    if (pid == "000c10" && type == "Fan") return "cp.f" + index;
    if (pid.rfind("001c", 0) == 0 && type == "Temperature") return "psu.tvrm";
    if (pid.rfind("001c", 0) == 0 && type == "Fan") return "psu.fan";
    if (pid == "000002" && type == "TemperaturePackage") return "cpu.tctl";
    if (pid == "000007" && type == "Temperature") return "gpu.temp";
    return std::string();
}

struct IcueConfig {
    std::string guid, name;
    bool predefined = false, psu = false;
    int fanMode = 0;
    float fixedPercents = 40, fixedRpm = 500;
    std::vector<CurvePoint> points;
    std::string sensor;
};

// Collects every cooling configuration defined anywhere in the tree.
void collectConfigs(const Node* root, std::map<std::string, IcueConfig>& out) {
    walk(root, [&](const Node* n) {
        const Node* base = n->child("base");
        if (n->tag != "data" || !base || !n->child("fanMode") || base->value("id").empty()) return;
        IcueConfig c;
        c.guid = base->value("id");
        c.name = base->value("name");
        c.predefined = base->value("predefined") == "true";
        c.fanMode = atoi(n->value("fanMode").c_str());
        c.fixedPercents = (float)atof(n->value("fixedPercents").c_str());
        c.fixedRpm = n->child("fixedRpm") ? (float)atof(n->value("fixedRpm").c_str()) : 0;
        c.sensor = n->value("sensorId");
        if (const Node* pts = n->child("customModePoints"))
            for (auto& p : pts->children) {
                if (p->value("enabled") != "true") continue;
                bool percent = p->child("rpmPercents") != nullptr;
                c.psu = c.psu || percent;
                c.points.push_back({(float)atof(p->value("temperature").c_str()),
                                    (float)atof(p->value(percent ? "rpmPercents" : "rpm").c_str())});
            }
        if (c.sensor.find("pid<001c") != std::string::npos) c.psu = true;
        if (c.psu && c.fanMode < 3) c.name = "PSU auto";  // iCUE's "Default": the PSU's own fan logic
        if (!out.count(c.guid)) out[c.guid] = c;
    });
}

bool toCurve(const IcueConfig& c, Curve& curve) {
    curve = Curve();
    curve.name = c.name;
    curve.sensor = mapSensor(c.sensor);
    curve.unit = c.psu ? UNIT_PERCENT : UNIT_RPM;
    switch (c.fanMode) {
    case 3:  // fixed percent
        curve.mode = MODE_FIXED;
        curve.unit = UNIT_PERCENT;
        curve.fixed = c.fixedPercents;
        return true;
    case 4:  // fixed rpm
        curve.mode = MODE_FIXED;
        curve.unit = UNIT_RPM;
        curve.fixed = c.fixedRpm;
        return true;
    case 5:  // custom curve
        if (c.points.size() < 2) return false;
        curve.mode = MODE_CURVE;
        curve.points = c.points;
        curve.sortPoints();
        return true;
    default:  // Quiet / Balanced / Extreme / Default presets: firmware or iCUE internals
        if (!c.psu) return false;
        curve.mode = MODE_DEVICE;
        return true;
    }
}

}  // namespace

std::string importIcue(const std::string& folder, Config& config) {
    std::string log;
    char line[512];
    auto configsXml = loadXml(folder + "/cooling/configurations");
    if (!configsXml) return std::string();
    std::map<std::string, IcueConfig> configs;
    collectConfigs(configsXml.get(), configs);

    // Names: UserProps entries point at sensors through cereal pointer ids.
    if (auto props = loadXml(folder + "/sensors/UserProps")) {
        std::map<std::string, std::string> pointerToId;
        walk(props.get(), [&](const Node* n) {
            if (n->tag != "ptr_wrapper") return;
            const Node* data = n->child("data");
            if (data && !data->value("id").empty()) pointerToId[n->value("id")] = data->value("id");
        });
        walk(props.get(), [&](const Node* n) {
            const Node* sensor = n->child("sensor");
            if (!sensor || !n->child("name") || !sensor->child("ptr_wrapper")) return;
            std::string ours = mapSensor(pointerToId[sensor->child("ptr_wrapper")->value("id")]);
            std::string name = n->value("name");
            if (ours.empty() || name.empty() || ours.rfind("cp.", 0) != 0) return;
            config.names[ours] = name;
            snprintf(line, sizeof line, "name   %-8s = %s\n", ours.c_str(), name.c_str());
            log += line;
        });
    }

    // Profiles: names from the profile tree, assignments from each profile's data.
    std::vector<std::pair<std::string, std::string>> profiles;  // name, guid
    if (auto tree = loadXml(folder + "/profiles/tree.cueprofileorder"))
        walk(tree.get(), [&](const Node* n) {
            const Node* base = n->child("base");
            if (n->tag == "data" && base && base->value("isFolder") == "false" && !n->value("id").empty())
                profiles.push_back({base->value("name"), n->value("id")});
        });

    std::set<std::string> used;
    std::vector<Profile> imported;
    for (auto& entry : profiles) {
        auto data = loadXml(folder + "/profiles/" + entry.second + ".cueprofiledata");
        if (!data) continue;
        collectConfigs(data.get(), configs);
        Profile profile;
        profile.name = entry.first.empty() ? "iCUE" : entry.first;
        walk(data.get(), [&](const Node* n) {
            if (n->tag != "assignments") return;
            for (auto& a : n->children) {
                std::string output = mapSensor(a->value("key"));
                const Node* v = a->child("value");
                if (output.empty() || !v) continue;
                std::string guid = v->value("assignmentConfigId");
                auto it = configs.find(guid);
                if (it == configs.end()) continue;
                Curve probe;
                if (!toCurve(it->second, probe)) continue;
                profile.assignments[output] = it->second.name;
                used.insert(guid);
                snprintf(line, sizeof line, "assign %-8s -> %s (profile %s)\n", output.c_str(),
                         it->second.name.c_str(), profile.name.c_str());
                log += line;
            }
        });
        imported.push_back(profile);
    }

    // Curves: user-made ones plus any preset a profile actually uses.
    for (auto& kv : configs) {
        const IcueConfig& c = kv.second;
        if (c.predefined && !used.count(kv.first)) continue;
        Curve curve;
        if (!toCurve(c, curve)) continue;
        if (Curve* existing = config.findCurve(curve.name)) {
            if (c.predefined) continue;  // keep the user's version
            *existing = curve;
        } else {
            config.curves.push_back(curve);
        }
        snprintf(line, sizeof line, "curve  %-14s %s %s sensor=%s points=%d\n", curve.name.c_str(),
                 curve.mode == MODE_CURVE ? "curve" : (curve.mode == MODE_FIXED ? "fixed" : "device"),
                 curve.unit == UNIT_RPM ? "rpm" : "%", curve.sensor.c_str(), (int)curve.points.size());
        log += line;
    }
    for (Profile& p : imported) {
        bool replaced = false;
        for (Profile& existing : config.profiles)
            if (existing.name == p.name) {
                existing = p;
                replaced = true;
            }
        if (!replaced) config.profiles.push_back(p);
        if (config.activeProfile.empty()) config.activeProfile = p.name;
    }
    if (!imported.empty()) config.activeProfile = imported.front().name;
    return log.empty() ? std::string("nothing found\n") : log;
}
