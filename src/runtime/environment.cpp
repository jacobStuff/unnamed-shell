#include "runtime/environment.hpp"

#include <unistd.h>

// POSIX guarantees this exists but not that any header declares it.
extern char** environ;

namespace ush {

Environment::Environment(std::string shellNameIn)
    : shellName(std::move(shellNameIn)), shellPid(static_cast<int>(getpid())) {
    for (char** e = environ; e && *e; ++e) {
        std::string entry(*e);
        auto eq = entry.find('=');
        if (eq == std::string::npos) continue;  // malformed entry; ignore
        Variable v;
        v.value = entry.substr(eq + 1);
        v.exported = true;
        vars_.emplace(entry.substr(0, eq), std::move(v));
    }
}

std::optional<std::string> Environment::get(const std::string& name) const {
    auto it = vars_.find(name);
    if (it == vars_.end()) return std::nullopt;
    return it->second.value;
}

bool Environment::isSet(const std::string& name) const { return vars_.count(name) != 0; }

bool Environment::isExported(const std::string& name) const {
    auto it = vars_.find(name);
    return it != vars_.end() && it->second.exported;
}

bool Environment::isReadonly(const std::string& name) const {
    auto it = vars_.find(name);
    return it != vars_.end() && it->second.readonly;
}

void Environment::set(const std::string& name, std::string value) {
    auto it = vars_.find(name);
    if (it != vars_.end()) {
        if (it->second.readonly) throw ReadonlyVariableError(name);
        it->second.value = std::move(value);
        return;
    }
    Variable v;
    v.value = std::move(value);
    vars_.emplace(name, std::move(v));
}

void Environment::setExported(const std::string& name, bool exported) {
    vars_[name].exported = exported;
}

void Environment::setReadonly(const std::string& name, bool readonly) {
    vars_[name].readonly = readonly;
}

void Environment::unset(const std::string& name) { vars_.erase(name); }

std::string Environment::ifsOrDefault() const {
    auto v = get("IFS");
    return v ? *v : " \t\n";
}

std::vector<std::string> Environment::exportedEnviron() const {
    std::vector<std::string> result;
    result.reserve(vars_.size());
    for (const auto& [name, var] : vars_) {
        if (var.exported) result.push_back(name + "=" + var.value);
    }
    return result;
}

}  // namespace ush
