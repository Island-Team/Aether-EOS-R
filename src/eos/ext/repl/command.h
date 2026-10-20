/// Northbound REPL input parsing — one input line into an Action, pure of
/// sockets and I/O. Friendly commands map onto GatewayService RPCs. The parser
/// mints command IDs so primitive execution requests correlate with receipts.
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace eos::ext::repl {

/// What one input line asks for once parsed.
struct Action {
  /// The dispatch kinds the prompt loop switches on.
  enum class Kind {
    kNone,         ///< empty line or `#` comment — do nothing
    kError,        ///< parse failure; `error` carries the message
    kHelp,         ///< local cheat-sheet
    kQuit,         ///< `quit` / `exit` — stop launcher-owned eosd and this REPL
    kLeave,        ///< `leave` — exit only this REPL, preserving launcher services
    kShutdown,     ///< `shutdown` — stop the launcher-owned eosd and this REPL
    kRebuild,      ///< `rebuild` — ask the launcher to rebuild and restart eosd + REPL
    kConnect,      ///< `connect [host] [port]`
    kDisconnect,   ///< `disconnect` — close the session, stay in the REPL
    kRequest,      ///< friendly command → `method` + `params` round-trip
    kWatch,        ///< `watch <uri>` — live view plus auto re-read on push
    kWatchStream,  ///< modal Skill execution/event SSE view
    kWorkflowWatch,  ///< snapshot then watch ActionList events
    kDescribe,     ///< local primitive or remote skill parameter description
    kSetFull,       ///< `set full on|off` — toggle base64 elision
    kSetBrief,      ///< `set brief on|off` — toggle concise rendering
    kRefresh,       ///< refresh runtime-backed completion metadata
    kAutoBase,      ///< `auto base [device]` — keyboard stream navigation
  };

  Kind kind = Kind::kNone;
  std::string method;          ///< kRequest: GatewayService RPC name
  nlohmann::json params;       ///< kRequest: params object (null = omit)
  std::string uri;             ///< read/watch target
  std::string stream_path;     ///< kWatchStream: adapter SSE target
  std::string stream_key;      ///< kWatchStream: cancellation identity
  std::string describe_kind;   ///< kDescribe: primitive or skill
  std::string host;            ///< kConnect override; empty = default
  std::uint16_t port = 0;      ///< kConnect override; 0 = default
  bool full = false;           ///< kSetFull target state (true = show verbatim)
  bool brief = false;          ///< kSetBrief target state
  bool prompt_json = false;    ///< request arguments from the multiline JSON editor
  bool guided_prompt = false;  ///< use schema-guided fields instead of raw JSON entry
  bool strict_json = false;    ///< inline --json must validate without inferred prompts
  bool attach_execution = false;  ///< attach to a successfully started execution
  std::string help_topic;      ///< optional command named by `help <command>`
  std::string history_prefix;  ///< canonical prefix used for edited JSON history
  std::string error;           ///< kError message
};

/// Parses REPL input lines and mints globally unique command identities.
class CommandParser final {
 public:
  /// Parses one input line (leading/trailing whitespace ignored).
  Action Parse(const std::string& line);
};

}  // namespace eos::ext::repl
