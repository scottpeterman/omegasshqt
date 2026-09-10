// sessions/sessionio.cpp

#include "sessionio.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace omega::sessions {
namespace {

std::string jsonEscape(const std::string &s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char *hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Reads one string out of a flat JSON object. Deliberately not a parser: it
// finds "key" at the top level and returns the string that follows. Nested
// objects and non-string values read as absent, which is the honest answer for
// a reader this small.
bool jsonFindString(const std::string &json, const std::string &key,
                    std::string *out) {
    const std::string needle = "\"" + jsonEscape(key) + "\"";
    size_t depth = 0;
    bool inString = false;
    bool escaped = false;

    for (size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (inString) {
            if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            // Only consider keys at the object's top level.
            if (depth == 1 && json.compare(i, needle.size(), needle) == 0) {
                size_t j = i + needle.size();
                while (j < json.size() && std::isspace(static_cast<unsigned char>(json[j])))
                    ++j;
                if (j >= json.size() || json[j] != ':') { inString = true; continue; }
                ++j;
                while (j < json.size() && std::isspace(static_cast<unsigned char>(json[j])))
                    ++j;
                if (j >= json.size() || json[j] != '"') return false;
                ++j;
                std::string value;
                bool esc = false;
                for (; j < json.size(); ++j) {
                    const char d = json[j];
                    if (esc) {
                        switch (d) {
                            case 'n': value.push_back('\n'); break;
                            case 't': value.push_back('\t'); break;
                            case 'r': value.push_back('\r'); break;
                            case '"': value.push_back('"'); break;
                            case '\\': value.push_back('\\'); break;
                            default: value.push_back(d);
                        }
                        esc = false;
                        continue;
                    }
                    if (d == '\\') { esc = true; continue; }
                    if (d == '"') break;
                    value.push_back(d);
                }
                *out = value;
                return true;
            }
            inString = true;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
    }
    return false;
}

int portOf(const TtDevice &device) {
    // int(sess.get("port", 22)) in the Python, which accepts "22" as readily as
    // 22 because YAML may have given it either. A value that is not a number at
    // all falls back to 22 rather than refusing the whole import over one row.
    const std::string *raw = device.find("port");
    if (!raw || raw->empty()) return 22;
    char *end = nullptr;
    const long value = std::strtol(raw->c_str(), &end, 10);
    if (end == raw->c_str() || value <= 0 || value > 65535) return 22;
    return static_cast<int>(value);
}

std::string valueOr(const TtDevice &device, const char *key) {
    const std::string *found = device.find(key);
    return found ? *found : std::string();
}

}  // namespace

std::string extrasGet(const std::string &extras, const std::string &key) {
    std::string out;
    if (!jsonFindString(extras, key, &out)) return {};
    return out;
}

std::string extrasBuild(
    const std::vector<std::pair<std::string, std::string>> &fields) {
    if (fields.empty()) return "{}";
    std::string out = "{";
    bool first = true;
    for (const auto &[key, value] : fields) {
        if (!first) out += ", ";
        out += "\"" + jsonEscape(key) + "\": \"" + jsonEscape(value) + "\"";
        first = false;
    }
    out += "}";
    return out;
}

ImportResult applyImport(SessionStore &store, const TtDocument &doc,
                         ImportMode mode, Status *status) {
    ImportResult result;
    if (status) *status = Status::good();

    // Keyed by hostname, as the Python is. Built once and kept current as rows
    // are added, so two YAML entries sharing a host inside one file behave the
    // same as one that collided with a row already in the store.
    std::map<std::string, Session> byHost;
    for (const Session &s : store.listAllSessions()) byHost[s.hostname] = s;

    for (const TtFolder &folder : doc.folders) {
        if (folder.devices.empty()) continue;  // an empty folder makes nothing

        const std::string folderName =
            folder.name.empty() ? std::string("Imported") : folder.name;

        std::optional<int64_t> folderId;
        for (const Folder &existing : store.listFolders(std::nullopt)) {
            if (existing.name == folderName) {
                folderId = existing.id;
                break;
            }
        }
        if (!folderId.has_value()) {
            Status addStatus;
            folderId = store.addFolder(folderName, std::nullopt, &addStatus);
            if (!addStatus) {
                if (status) *status = addStatus;
                return result;
            }
            ++result.foldersCreated;
        }

        for (const TtDevice &device : folder.devices) {
            const std::string host = valueOr(device, "host");
            if (host.empty()) continue;  // no address, nothing to connect to

            const auto existing = byHost.find(host);
            if (existing != byHost.end() && mode == ImportMode::Skip) {
                ++result.sessionsSkipped;
                continue;
            }

            const std::string deviceType = valueOr(device, "DeviceType");
            const std::string model = valueOr(device, "Model");
            const std::string vendor = valueOr(device, "Vendor");

            std::string description = deviceType;
            if (!deviceType.empty() && !model.empty()) description += " - ";
            description += model;

            std::vector<std::pair<std::string, std::string>> extras;
            if (!vendor.empty()) extras.emplace_back("vendor", vendor);
            if (!deviceType.empty()) extras.emplace_back("device_type", deviceType);
            if (!model.empty()) extras.emplace_back("model", model);

            Session session;
            const std::string display = valueOr(device, "display_name");
            session.name = display.empty() ? host : display;
            session.description = description;
            session.hostname = host;
            session.port = portOf(device);
            session.credential_name.reset();  // agent auth, as the Python does
            session.folder_id = folderId;
            // NOTE: replaced, not merged. Any other key this session was
            // carrying in extras is dropped, which is what update_session does
            // with the dict the Python builds here.
            session.extras = extrasBuild(extras);

            if (existing != byHost.end()) {
                // Start from the ROW, not from the YAML. updateSession writes
                // every writable column, so building a fresh Session here and
                // saving it would NULL every per-session override the row had
                // -- term type, host key policy, anti-idle, the lot -- and
                // reset its transport to ssh. The import owns six fields; the
                // rest of the row is none of its business.
                Session merged = existing->second;
                merged.name = session.name;
                merged.description = session.description;
                merged.hostname = session.hostname;
                merged.port = session.port;
                merged.credential_name = session.credential_name;
                merged.folder_id = session.folder_id;
                merged.extras = session.extras;
                session = merged;

                const Status wrote = store.updateSession(session);
                if (!wrote) {
                    if (status) *status = wrote;
                    return result;
                }
            } else {
                Status addStatus;
                const auto id = store.addSession(session, &addStatus);
                if (!addStatus) {
                    if (status) *status = addStatus;
                    return result;
                }
                session.id = id;
            }

            byHost[host] = session;
            ++result.sessionsImported;
        }
    }

    return result;
}

TtDocument buildExport(SessionStore &store, Status *status) {
    TtDocument doc;
    if (status) *status = Status::good();

    Status treeStatus;
    const Tree tree = store.tree(&treeStatus);
    if (!treeStatus) {
        if (status) *status = treeStatus;
        return doc;
    }

    // Full path per folder, since the format has no nesting. Built by walking
    // parents, with a depth cap so a cycle in a hand-edited file cannot hang
    // the export.
    std::map<int64_t, Folder> folders;
    for (const Folder &f : tree.folders)
        if (f.id.has_value()) folders[*f.id] = f;

    auto pathOf = [&folders](int64_t id) {
        std::vector<std::string> parts;
        std::optional<int64_t> current = id;
        for (int depth = 0; current.has_value() && depth < 64; ++depth) {
            const auto it = folders.find(*current);
            if (it == folders.end()) break;
            parts.push_back(it->second.name);
            current = it->second.parent_id;
        }
        std::reverse(parts.begin(), parts.end());
        std::string path;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) path += "/";
            path += parts[i];
        }
        return path;
    };

    // One entry per folder that has sessions, plus Ungrouped, in tree order so
    // the file reads the way the tree looks.
    std::vector<std::pair<std::string, std::vector<const Session *>>> grouped;
    auto bucketFor = [&grouped](const std::string &name)
        -> std::vector<const Session *> & {
        for (auto &[key, list] : grouped)
            if (key == name) return list;
        grouped.emplace_back(name, std::vector<const Session *>{});
        return grouped.back().second;
    };

    for (const Folder &f : tree.folders) {
        if (!f.id.has_value()) continue;
        bool any = false;
        for (const Session &s : tree.sessions)
            if (s.folder_id == f.id) { any = true; break; }
        if (!any) continue;  // the format has no way to say "empty folder"
        std::vector<const Session *> &bucket = bucketFor(pathOf(*f.id));
        for (const Session &s : tree.sessions)
            if (s.folder_id == f.id) bucket.push_back(&s);
    }

    std::vector<const Session *> loose;
    for (const Session &s : tree.sessions)
        if (!s.folder_id.has_value()) loose.push_back(&s);
    if (!loose.empty()) {
        std::vector<const Session *> &bucket = bucketFor("Ungrouped");
        bucket.insert(bucket.end(), loose.begin(), loose.end());
    }

    for (const auto &[name, list] : grouped) {
        TtFolder folder;
        folder.name = name;
        for (const Session *s : list) {
            TtDevice device;
            device.set("display_name", s->name);
            device.set("host", s->hostname);
            device.set("port", std::to_string(s->port));

            // Only the keys this format defines, read back out of the extras
            // the importer wrote. A session created by hand has none of them
            // and exports with the three fields above alone.
            const std::string vendor = extrasGet(s->extras, "vendor");
            const std::string deviceType = extrasGet(s->extras, "device_type");
            const std::string model = extrasGet(s->extras, "model");
            if (!deviceType.empty()) device.set("DeviceType", deviceType);
            if (!model.empty()) device.set("Model", model);
            if (!vendor.empty()) device.set("Vendor", vendor);

            folder.devices.push_back(std::move(device));
        }
        doc.folders.push_back(std::move(folder));
    }

    return doc;
}

}  // namespace omega::sessions
