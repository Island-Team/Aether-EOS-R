"""Terminal operator console for a running EGIA pipeline.

Out-of-process like every :mod:`egia_ext` package: the TUI never imports ``egia``
implementation modules. It watches the northbound WebSocket surface
(``/api/aether/egia/health`` for subsystem status, ``/context`` for the live
prompt snapshot) and follows the active Island session log when supervised,
falling back to ``var/lib/egia/egia.jsonl`` for standalone EGIA.

Panes: main (left) — the assembled context prompt; top-right — EGIA process
status; right-bottom — error/warning log stream.
"""
