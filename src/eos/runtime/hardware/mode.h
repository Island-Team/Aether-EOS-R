/// runtime/hardware — active-mode parsing: the `[mode]` section of
/// etc/eos/eos.toml into the hardware selection the supervisor hands the bridge.
/// The single `[[mode.binding]]` names one binding file and required profile.
/// Additional robots run as separate EOS processes on separate local ports. The section also
/// carries an operator-primitive exposure flag. Missing or malformed values
/// throw std::runtime_error naming the file and the problem; the only
/// silent default is `expose_operator = false` when the primitives table omits
/// it (a safety-relevant off-by-default).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace eos::runtime::hardware_runtime {

/// The hardware profile selected by the process's `[[mode.binding]]` entry.
struct ModeBinding {
  std::string backend;  ///< binding path under etc/, e.g. "backends/demo.toml"
  std::string profile;  ///< required [profile.<name>] inside the binding
};

/// The active launch mode: exactly one hardware binding and shared primitive policy.
struct ModeConfig {
  std::vector<ModeBinding> bindings;  ///< exactly one entry
  bool expose_operator{false};        ///< [mode.primitives] expose_operator
};

/// Parses config text for the `[mode]` section. `path` names the file in
/// error messages. Throws std::runtime_error when the section is absent,
/// exactly one `[[mode.binding]]` does not exist, a binding key is missing/empty/wrongly typed, or
/// `expose_operator` is not a boolean. Sibling keys such as `io_debug`
/// are composition concerns of other modules and are ignored here.
ModeConfig ParseMode(std::string_view text, const std::string& path);

}  // namespace eos::runtime::hardware_runtime
