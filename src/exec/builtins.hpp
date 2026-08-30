// Built-in utilities: special (§2.14, run in-process, assignments on the
// same command line persist afterward, don't fork) and a practical
// subset of regular built-ins (also run in-process, but assignments are
// temporary like any other command - see Executor::runSimpleCommand).
//
// Not yet implemented (documented gaps - see docs/DESIGN.md): `times`,
// `trap` (no real signal handling yet), `kill`, `hash`, `alias`/
// `unalias`, job control (`jobs`/`bg`/`fg` - `wait` exists but is
// best-effort with no real job table).

#pragma once

#include <string>
#include <vector>

namespace ush {

class Executor;

bool isSpecialBuiltin(const std::string& name);
bool isBuiltin(const std::string& name);  // true for either special or regular

// Runs the builtin named `name` (which must satisfy isBuiltin(name)).
// `args[0]` is the command name itself, as in argv.
int callBuiltin(const std::string& name, Executor& ex, const std::vector<std::string>& args);

}  // namespace ush
