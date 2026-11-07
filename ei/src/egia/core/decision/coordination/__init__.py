"""Coordination — the §3.5 primitive market over profile steps.

Pure decision machinery: bidder profiles compiled from backend primitive
advertisements, the epsilon-raising award loop, the task board that tracks
profile steps through award and completion, and zone leases that arbitrate
shared workspaces. The supervisor entry and executor fan-out live in
``coordinator.py``; everything here is deterministic and side-effect-free
beyond its own state, so the market is unit-testable without a backend.
"""
