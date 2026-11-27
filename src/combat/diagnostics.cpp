#include "combat/diagnostics.h"

#include <cctype>
#include <string>
#include <utility>

namespace hailstone::combat {

namespace {

void append_field(std::string& out, std::string_view key, std::string_view value) {
    if (value.empty()) return;
    out += ' ';
    out += key;
    out += '=';
    // Keep the line one field-per-line parseable: no separators inside a value.
    for (char c : value) out += (c == '\n' || c == '\r' || c == '\t' || c == '|') ? ' ' : c;
}

} // namespace

std::string format_diagnostic(const DiagnosticRecord& r) {
    std::string out;
    out.reserve(160);
    out += "char=";
    out += r.character.empty() ? "?" : r.character;
    append_field(out, "server", r.server);
    append_field(out, "class", r.class_short);
    append_field(out, "group", r.group_id);
    append_field(out, "module", diagnostic_module_name(r.module));
    append_field(out, "action", r.action);
    append_field(out, "target", r.target);
    append_field(out, "result", r.ok ? "ok" : "failed");
    append_field(out, "reason", r.reason);
    append_field(out, "state", r.state);
    if (r.occurrences > 1) append_field(out, "repeats", std::to_string(r.occurrences));
    return out;
}

void DiagnosticsSink::set_identity(std::string character,
                                   std::string server,
                                   std::string class_short,
                                   std::string group_id) {
    character_ = std::move(character);
    server_ = std::move(server);
    class_short_ = std::move(class_short);
    group_id_ = std::move(group_id);
}

DiagnosticRecord* DiagnosticsSink::find_dedup_target(const DiagnosticRecord& candidate, std::uint64_t now_ms) {
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (now_ms < it->last_ms) continue; // tick went backwards; not a repeat
        if (now_ms - it->last_ms > options_.dedupe_window_ms) break; // too old to fold
        if (it->module != candidate.module) continue;
        if (it->ok != candidate.ok) continue;
        if (it->action != candidate.action) continue;
        if (it->target != candidate.target) continue;
        if (it->reason != candidate.reason) continue;
        if (it->severity != candidate.severity) continue;
        return &(*it);
    }
    return nullptr;
}

const DiagnosticRecord& DiagnosticsSink::emit(DiagnosticModule module,
                                              DiagnosticSeverity severity,
                                              std::string_view action,
                                              std::string_view target,
                                              bool ok,
                                              std::string_view reason,
                                              std::string_view state,
                                              std::uint64_t now_ms) {
    DiagnosticRecord candidate;
    candidate.first_ms = now_ms;
    candidate.last_ms = now_ms;
    candidate.character = character_;
    candidate.server = server_;
    candidate.class_short = class_short_;
    candidate.group_id = group_id_;
    candidate.module = module;
    candidate.severity = severity;
    candidate.action = std::string(action);
    candidate.target = std::string(target);
    candidate.reason = std::string(reason);
    candidate.state = std::string(state);
    for (char& c : candidate.state) {
        // The log line is a lowercase key=value tag list; the states are named
        // for display ("Idle") and are tagged as "idle" here.
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    candidate.ok = ok;

    if (DiagnosticRecord* existing = find_dedup_target(candidate, now_ms)) {
        existing->last_ms = now_ms;
        ++existing->occurrences;
        ++existing->suppressed_repeats;
        ++suppressed_total_;
        // A later occurrence may carry fresher state text than the first.
        if (!candidate.state.empty()) existing->state = std::move(candidate.state);
        return *existing;
    }

    if (options_.max_records > 0 && records_.size() >= options_.max_records) {
        records_.pop_front();
    }
    records_.push_back(std::move(candidate));
    return records_.back();
}

bool DiagnosticsSink::emit_unsupported_command(std::string_view command, std::uint64_t now_ms) {
    const std::string key(command);
    if (command_warned(key)) {
        // Bump the record if it is still in the window list; otherwise stay quiet.
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
            if (it->module != DiagnosticModule::Command) continue;
            if (it->target != key) continue;
            it->last_ms = now_ms;
            ++it->occurrences;
            ++it->suppressed_repeats;
            ++suppressed_total_;
            break;
        }
        return false;
    }
    warned_commands_.push_back(key);
    (void)emit(DiagnosticModule::Command, DiagnosticSeverity::Warning, "UnsupportedCommand", command, false,
               "not implemented by the native stack", "command", now_ms);
    return true;
}

bool DiagnosticsSink::command_warned(std::string_view command) const {
    for (const auto& c : warned_commands_) {
        if (c == command) return true;
    }
    return false;
}

std::vector<DiagnosticRecord> DiagnosticsSink::records() const {
    return std::vector<DiagnosticRecord>(records_.begin(), records_.end());
}

std::vector<DiagnosticRecord> DiagnosticsSink::records(DiagnosticModule module) const {
    std::vector<DiagnosticRecord> out;
    for (const auto& r : records_) {
        if (r.module == module) out.push_back(r);
    }
    return out;
}

std::string DiagnosticsSink::last_line() const {
    if (records_.empty()) return {};
    return format_diagnostic(records_.back());
}

void DiagnosticsSink::clear() {
    records_.clear();
    suppressed_total_ = 0;
}

} // namespace hailstone::combat
