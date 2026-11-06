"""dotenv loader — file as fallback, environment as authority."""

from __future__ import annotations

from pathlib import Path

from egia.lib.dotenv import load_env_file
from egia.main import load_dotenv
from island.path import Layout


def _write(tmp_path: Path, body: str) -> Path:
  env = tmp_path / ".env"
  env.write_text(body, encoding="utf-8")
  return env


def test_parses_keys_comments_quotes_and_export(tmp_path, monkeypatch):
  for key in ("MOONSHOT_API_KEY", "EGIA_MODEL_API_KEY", "EMPTY"):
    monkeypatch.delenv(key, raising=False)
  env = _write(
    tmp_path,
    "# comment\n"
    "MOONSHOT_API_KEY=sk-test123\n"
    'export EGIA_MODEL_API_KEY="quoted value"\n'
    "BAD LINE\n"
    "EMPTY=\n",
  )
  loaded = load_env_file(env)
  assert sorted(loaded) == ["EGIA_MODEL_API_KEY", "EMPTY", "MOONSHOT_API_KEY"]
  import os

  assert os.environ["MOONSHOT_API_KEY"] == "sk-test123"
  assert os.environ["EGIA_MODEL_API_KEY"] == "quoted value"
  assert os.environ["EMPTY"] == ""


def test_environment_wins_over_file(tmp_path, monkeypatch):
  monkeypatch.setenv("MOONSHOT_API_KEY", "sk-from-env")
  env = _write(tmp_path, "MOONSHOT_API_KEY=sk-from-file\n")
  assert load_env_file(env) == []
  import os

  assert os.environ["MOONSHOT_API_KEY"] == "sk-from-env"


def test_missing_file_loads_nothing(tmp_path):
  assert load_env_file(tmp_path / "absent.env") == []


def test_load_dotenv_returns_names_without_writing_raw_stderr(
  tmp_path, monkeypatch, capsys
) -> None:
  layout = Layout.resolve(prefix=tmp_path, mode="source", environ={})
  etc_root = tmp_path / "etc/egia"

  monkeypatch.setattr("egia.main.resolve_etc_root", lambda *_args, **_kwargs: etc_root)
  monkeypatch.setattr(
    "egia.lib.dotenv.load_env_file",
    lambda path: ["MODEL_API_KEY"] if path == layout.prefix / ".env" else [],
  )

  assert load_dotenv(None, layout) == [(layout.prefix / ".env", ["MODEL_API_KEY"])]
  assert capsys.readouterr().err == ""
