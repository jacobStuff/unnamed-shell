// Shell variable / positional-parameter / special-parameter state, per
// POSIX.1-2017 Shell & Utilities §2.5 "Parameters and Variables".
//
// This is storage and lookup only - it's the substrate the expansion
// stage (§2.6) reads from and (for arithmetic assignment, and later the
// `set`/`export`/`readonly`/`unset` built-ins) writes to. It does not
// itself implement those built-ins.

#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace ush {

class ReadonlyVariableError : public std::runtime_error {
public:
    explicit ReadonlyVariableError(const std::string& name)
        : std::runtime_error("'" + name + "': is read only") {}
};

class Environment {
public:
    // Initializes shell variables from the real process environment (each
    // entry becomes an exported shell variable, per §2.5.3), and sets up
    // $0 and $$.
    explicit Environment(std::string shellName = "ush");

    // --- variables (§2.5.3) ---------------------------------------------
    std::optional<std::string> get(const std::string& name) const;
    bool isSet(const std::string& name) const;
    bool isExported(const std::string& name) const;
    bool isReadonly(const std::string& name) const;

    // Creates or updates `name`. Throws ReadonlyVariableError if it's
    // already marked readonly.
    void set(const std::string& name, std::string value);
    void setExported(const std::string& name, bool exported);
    // Throws ReadonlyVariableError if `name` is already readonly and
    // `readonly` is requested again with a different value pending -
    // matches the common shell behavior of allowing `readonly x` to be
    // re-stated but not `x=...` afterward; ush only enforces the latter
    // here (this just sets the flag).
    void setReadonly(const std::string& name, bool readonly);
    void unset(const std::string& name);

    // --- positional parameters ($1.., $#, $@, $*; §2.5.2) ---------------
    const std::vector<std::string>& positionalParams() const { return positional_; }
    void setPositionalParams(std::vector<std::string> params) { positional_ = std::move(params); }

    // --- special parameters (§2.5.2) ------------------------------------
    std::string shellName;                // $0
    int lastExitStatus = 0;                // $?
    std::optional<int> lastBackgroundPid;  // $!
    int shellPid = 0;                      // $$

    // IFS (§2.5.3), or the default <space><tab><newline> if unset.
    std::string ifsOrDefault() const;

    // "NAME=VALUE" for every exported variable, suitable for execve()'s
    // envp (used by the executor).
    std::vector<std::string> exportedEnviron() const;

private:
    struct Variable {
        std::string value;
        bool exported = false;
        bool readonly = false;
    };
    std::unordered_map<std::string, Variable> vars_;
    std::vector<std::string> positional_;
};

}  // namespace ush
