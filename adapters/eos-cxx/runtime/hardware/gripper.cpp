#include "class.h"

namespace eos::runtime::hardware_runtime::detail {
void ParseGripperPart(const toml::table& table, PartConfig& config, const std::string& path,
                      const std::string& where) {
  RequireNoPartKeys(table, config, path, where);
}
}  // namespace eos::runtime::hardware_runtime::detail
