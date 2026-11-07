/// Northbound REPL output rendering for north HTTP ProtoJSON payloads.
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace eos::ext::repl {

/// Display knobs the prompt loop owns and toggles at runtime.
struct DisplayOptions {
  /// Elide long base64 blobs (`image_b64`, `pcm_b64`, map `cells`) to a
  /// length note when explicitly enabled with `set full off`.
  bool elide_b64 = false;

  /// Apply ANSI syntax highlighting to JSON output.
  bool color = false;

  /// Render recognized resources and execution state as concise summaries.
  bool brief = false;
};

/// Renders one complete RPC result as a labeled, human-readable view.
std::string RenderReply(const std::string& method, const nlohmann::json& reply,
                        const DisplayOptions& options);

/// Renders one filtered snapshot of retained skill executions as a compact table.
std::string RenderExecutionPs(const nlohmann::json& reply, const nlohmann::json& filters,
                               const DisplayOptions& options);

/// Renders a payload body without an RPC heading, applying elision when enabled.
std::string FormatResult(const nlohmann::json& result, const DisplayOptions& options);

/// Renders a WatchResource update as `[update] <uri>`.
std::string FormatNotification(const nlohmann::json& params);

}  // namespace eos::ext::repl
