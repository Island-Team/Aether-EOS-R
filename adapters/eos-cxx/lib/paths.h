/// Resolution of the immutable and mutable filesystem roles used by EOS.
/// Launch-time only: the supervisor resolves them once and passes paths down
/// by value; runtime modules never inspect the current working directory.
#ifndef SRC_LIB_PATHS_H_
#define SRC_LIB_PATHS_H_

#include <filesystem>
#include <stdexcept>
#include <string>

namespace eos::lib {

/// Thrown when a required root cannot be resolved. what() names the resolved
/// candidates so the launch error is actionable.
class ConfigRootError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Absolute filesystem roles resolved for one EOS process.
struct LayoutPaths {
  std::filesystem::path etc;        ///< EOS configuration root.
  std::filesystem::path contracts;  ///< Canonical Agent-runtime contract root.
  std::filesystem::path state;      ///< Durable EOS state root.
  std::filesystem::path cache;      ///< Rebuildable EOS cache root.
  std::filesystem::path log;        ///< EOS diagnostic log root.
  std::filesystem::path run;        ///< Island boot-lifetime runtime root.
  std::filesystem::path tmp;        ///< Island disposable scratch root.
};

/// Resolves the configuration root: `cli_arg`, EOS_ETC, then
/// ISLAND_ETC/eos. Caller-controlled roots must be absolute.
std::filesystem::path ResolveEtcRoot(const std::string& cli_arg);

/// Resolves the canonical contract root: `cli_arg`, EOS_CONTRACTS, then
/// ISLAND_SHARE. Caller-controlled roots must be absolute.
std::filesystem::path ResolveContractsRoot(const std::string& cli_arg);

/// Resolves every EOS filesystem role and creates the mutable role roots.
/// ISLAND_STATE/CACHE/LOG are product roots and receive an `eos` namespace;
/// ISLAND_RUN/TMP are already final Island roots. EOS_VAR remains a legacy
/// compatibility root. Throws ConfigRootError on missing or invalid roots.
LayoutPaths ResolveLayoutPaths(const std::string& cli_etc, const std::string& cli_contracts);

}  // namespace eos::lib

#endif  // SRC_LIB_PATHS_H_
