"""The §3.5 epsilon-raising award loop over the ready set.

Cost-minimization auction: steps are items, bidders are executors, and a
bidder's price is compensation it demands on top of its raw bid. Each round
every unassigned step picks its cheapest feasible bidder; a bidder chosen
by exactly one step is awarded immediately, while a bidder contested by
several steps has its price raised (each contender's gap to its own
runner-up, plus epsilon) and the contenders re-bid next round. With n steps
and m bidders the loop is O(nm/epsilon) bids and the classical bound
applies: the result is within n*epsilon of the optimal assignment, so
epsilon is a direct optimality/latency dial. Feasibility is upstream — the
auction only ever sees bids that were actually made (abstention already
filtered the infeasible), so every constructed assignment is structurally
feasible.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from egia.core.decision.coordination.bidders import BidderProfile
from egia.core.decision.profiles import ProfileStep

__all__ = ["AuctionResult", "award"]

_MAX_ROUNDS = 64


@dataclass(frozen=True)
class Bid:
  """One recorded bid: a step, a bidder, and the raw (pre-price) cost."""

  step_id: str
  bidder_id: str
  price: float


@dataclass
class AuctionResult:
  """One auction's outcome: awards, standing prices, bids, abstainers."""

  awards: dict[str, str] = field(default_factory=dict)
  prices: dict[str, float] = field(default_factory=dict)
  bids: list[Bid] = field(default_factory=list)
  unassigned: list[str] = field(default_factory=list)


def award(
  steps: list[ProfileStep],
  bidders: list[BidderProfile],
  *,
  epsilon: float = 0.25,
  congestion: float = 0.1,
) -> AuctionResult:
  """Assign each step one distinct bidder via epsilon raising.

  Steps with no feasible bidder land in ``unassigned`` — the market never
  invents feasibility; the coordinator retries them on a later tick (a
  bidder may recover, or settled load may free capacity). Ties break on
  (net cost, bidder id) so the award is deterministic for the same inputs.
  """
  result = AuctionResult(prices={bidder.id: 0.0 for bidder in bidders})
  steps_by_id = {step.id: step for step in steps}
  pending = list(steps_by_id)
  # Bidders holding one award this invocation are busy for the rest of it —
  # one executing step per executor per tick; excess steps queue for the
  # next auction (a completion event re-opens capacity).
  busy: set[str] = set()

  for _round in range(_MAX_ROUNDS):
    # Each pending step bids: cheapest (raw bid + standing price) bidder.
    choice: dict[str, tuple[str, float, float]] = {}  # step -> (bidder, net, gap)
    for step_id in pending:
      step = steps_by_id[step_id]
      options: list[tuple[float, float, str]] = []  # (net, raw, bidder)
      for bidder in bidders:
        if bidder.id in busy:
          continue
        raw = bidder.bid(step, congestion=congestion)
        if raw is not None:
          options.append((raw + result.prices[bidder.id], raw, bidder.id))
      if not options:
        result.unassigned.append(step_id)
        continue
      options.sort(key=lambda entry: (entry[0], entry[2]))
      best_net, best_raw, best_bidder = options[0]
      second_net = options[1][0] if len(options) > 1 else best_net
      result.bids.append(Bid(step_id, best_bidder, best_raw))
      choice[step_id] = (best_bidder, best_net, second_net - best_net)
    pending = [step_id for step_id in pending if step_id in choice]
    if not pending:
      break
    # Contested bidders (chosen by several steps) award their cheapest
    # contender now and go busy; the other contenders re-bid next round
    # against the raised price. Uncontested choices are awarded directly.
    choosers: dict[str, list[str]] = {}
    for step_id, (bidder_id, _net, _gap) in choice.items():
      choosers.setdefault(bidder_id, []).append(step_id)
    contested = {bidder for bidder, held in choosers.items() if len(held) > 1}
    recontend: list[str] = []
    for bidder_id, held in choosers.items():
      if bidder_id in contested:
        winner = min(held, key=lambda sid: (choice[sid][1], sid))
        result.awards[winner] = bidder_id
        busy.add(bidder_id)
        # Raise by the tightest contender's gap plus epsilon — the classical
        # step that makes at least one contender switch or concede.
        raise_by = min(choice[sid][2] for sid in held) + epsilon
        result.prices[bidder_id] += raise_by
        recontend.extend(sid for sid in held if sid != winner)
      else:
        for step_id in held:
          result.awards[step_id] = bidder_id
        busy.add(bidder_id)
    if not recontend:
      break
    pending = recontend

  # Steps still pending after the round budget: no award this tick, retried
  # on the next one — same surface as abstention.
  result.unassigned.extend(
    step_id
    for step_id in pending
    if step_id not in result.awards and step_id not in result.unassigned
  )
  return result
