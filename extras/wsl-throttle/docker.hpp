#pragma once

#include <map>
#include <string>
#include <vector>

// Everything that knows the shape of the docker CLI. The agent above it only
// deals in Limits and Original.
namespace docker {

struct Original {
    long long nano = 0;
    long long shares = 0;
    long long mem = 0;
    long long swap = 0;
    bool mem_applied = false;
};

// A complete `docker update` limit set. `mem == 0` means "leave memory alone".
struct Limits {
    long long nano = 0;
    long long shares = 0;
    long long mem = 0;
    long long swap = 0;

    bool operator<(const Limits& other) const;
};

bool running_ids(std::vector<std::string>& ids, std::string& error);

// The exact `docker update` argv for a limit set. Exposed so tests can pin it:
// getting this wrong leaves containers capped with no way back.
std::vector<std::string> update_argv(const Limits& limits,
                                     const std::vector<std::string>& ids);

// Reads the limits currently set on `ids`. `memory_limited` records whether the
// caller intends to change memory, so the original is restored only then.
bool originals_of(const std::vector<std::string>& ids, bool memory_limited,
                  std::map<std::string, Original>& out, std::string& error);

int apply(const Limits& limits, const std::vector<std::string>& ids,
          std::vector<std::string>& errors);

// Restores each container to its recorded limits. Containers sharing identical
// originals go in one `docker update`, so the usual "everything was unlimited"
// case costs a single call. Returns the ids that were restored.
std::vector<std::string> release(const std::vector<std::string>& live,
                                 const std::map<std::string, Original>& originals,
                                 std::vector<std::string>& errors);

}  // namespace docker
