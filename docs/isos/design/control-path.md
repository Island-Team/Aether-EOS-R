# Keyboard-to-base control path

This page explains the current first control path for graph authors and implementers. It composes [generic node types](../interfaces/nodes/README.md), a [UI event](../interfaces/ui/node-events.md), and [core handoff](../interfaces/cores/README.md). The path demonstrates typed bridges; it is not a second node schema or a package-specific IsOS rule.

1. An IsosUI keyboard node selects one key or chord in its settings. While armed and focused, it publishes `pressed`, bounded `refresh`, and `released` states from its `state` output. Browser repeats do not multiply activation; focus loss and unmount release the hold.
2. The IsOS HTTP gateway is a separate registered bridge node. Its saved settings select an exact primitive binding path, three body velocity components (`x_m_s`, `y_m_s`, `yaw_rad_s`), and a 50–500 ms lease. A browser event supplies only key state, activation order, source path, and run fence. IsOS resolves the saved graph and converts the event into a typed movement request. Its command deadline allows 500 ms beyond the chosen lease for owner admission; the actuator lease itself does not grow.
3. The EOS HTTP gateway receives that request for the declared `movement` input. Its owner authenticates and fences it, then calls the EOS Dispatcher `base.velocity` primitive binding. Stream publication acknowledges admission without waiting for the whole lease to finish; the dispatcher remains the sole actuator admission point.
4. The primitive binding sends a typed command to the selected base hardware node. EOS owns the hardware adapter and MuJoCo child; the simulator is a generic verified package process from IsOS's perspective. The owner checks its own payload and stop semantics before readiness.
5. `released`, focus loss, disconnect, run replacement, and lease expiry all stop renewal. The owner must report terminal stop evidence; an uncertain physical stop is not reported as complete. The UI shows pending, accepted, terminal, or uncertain outcomes using run-fenced status.

Every edge is declared in the graph: UI producer → IsOS gateway → EOS gateway → primitive binding → hardware. The port data type and delivery mode must agree. Bridge capabilities allow the explicit HTTP transitions; they do not turn arbitrary edges into transport calls. The graph stores no bearer secret, endpoint URL, or unverified executable path. The [package design](packages.md) explains exact-pin resolution and [server design](server.md) explains the IsOS module boundary.

[中文](../../../docs_cn/isos/design/control-path_cn.md)
