/// Shared loopback core for the demo device backends — one implementation of
/// EOS-facing adapter metadata shared by the demo devices. Mock payloads,
/// state, integration, and halt behavior live in ext/simulator; this helper only
/// exposes identity/feed declarations and forwards release/health gating.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "eos/hardware/backend.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

/// Compiled-in admission data for one demo device; the fallback mirroring
/// the values in its `demo_<kind>` profile in etc/demo.toml. The bridge
/// registry builds devices from the parsed binding and checks these
/// declarations against the configured rate limits at admission.
struct DemoSpec {
  hardware::BackendIdentity identity;  ///< name "demo-<kind>", kind, device_id
  hardware::FeedDeclarations feeds;    ///< per-class rates, zeroed where unused
};

struct DemoHealth {
  bool alive{false};
  bool admitted{false};
};

/// The shared loopback adapter helper. Every accepted Execute overload funnels
/// through TryRelease. Halt is deliberately per adapter: several demo devices
/// may share one World, but stopping one device must not make its siblings
/// unavailable.
class Core {
 public:
  Core(DemoSpec spec, std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver);
  ~Core();

  /// Backs the shell's Identify() / DeclareFeeds().
  const hardware::BackendIdentity& identity() const;
  const hardware::FeedDeclarations& feeds() const;

  /// Gate for every accepted Execute overload: kAccepted while admitted and
  /// not halted, kRejected after Halt latches the safe envelope.
  hardware::ReleaseResult TryRelease();

  /// Latch the safe envelope; all further release is refused. Shells call
  /// their own Clamp() first — what to zero is kind-specific.
  void LatchHalt();

  /// Common health read: alive until halted.
  DemoHealth ReadHealth() const;

  bool halted() const;

 private:
  DemoSpec spec_;
  std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver_;
  std::atomic_bool halted_{false};
};

}  // namespace eos::backends::demo
