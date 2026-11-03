"""Selected versus installed MuJoCo release view regression checks."""

import copy
import hashlib
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path

from q01_selection import load_selection, selected_pins
from verify_q01_r06_a03 import release_views_equal, verify


FIXTURE = Path(__file__).parent / "fixtures" / "q01_r06_release_views.json"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_json(path, value):
    replacement = path.with_suffix(path.suffix + ".changed")
    replacement.write_text(json.dumps(value, sort_keys=True))
    replacement.replace(path)


class ReleaseViewTests(unittest.TestCase):
    def setUp(self):
        fixture = json.loads(FIXTURE.read_text())
        self.selected = fixture["selected"]
        self.installed = fixture["installed"]

    def test_current_nonempty_release_views_match_without_mutation(self):
        self.assertEqual(len(self.selected["assets"][0]["required_libraries"]), 68)
        self.assertEqual(self.selected["assets"][0]["tested_ubuntu"], ["24.04"])
        selected_before, installed_before = copy.deepcopy(self.selected), copy.deepcopy(self.installed)
        self.assertTrue(release_views_equal(self.selected, self.installed))
        self.assertEqual(self.selected, selected_before)
        self.assertEqual(self.installed, installed_before)

    def test_only_contract_empty_omissions_normalize(self):
        selected, installed = copy.deepcopy(self.selected), copy.deepcopy(self.installed)
        selected["assets"][0]["required_libraries"] = []
        selected["assets"][0]["tested_ubuntu"] = []
        installed["assets"][0].pop("required_libraries")
        installed["assets"][0].pop("tested_ubuntu")
        self.assertTrue(release_views_equal(selected, installed))

    def test_nonempty_declaration_changes_reject(self):
        for field, replacement in (
            ("required_libraries", ["libwrong.so"]),
            ("required_libraries", []),
            ("tested_ubuntu", ["22.04"]),
            ("tested_ubuntu", []),
        ):
            with self.subTest(field=field, replacement=replacement):
                installed = copy.deepcopy(self.installed)
                installed["assets"][0][field] = replacement
                self.assertFalse(release_views_equal(self.selected, installed))
                del installed["assets"][0][field]
                self.assertFalse(release_views_equal(self.selected, installed))

    def test_unexpected_nonempty_data_types_rejects(self):
        installed = copy.deepcopy(self.installed)
        installed["node_contributions"]["data_types"] = [{"id": "unexpected"}]
        self.assertFalse(release_views_equal(self.selected, installed))


@unittest.skipUnless(os.environ.get("Q01_R06_ARCHIVE"),
                     "set Q01_R06_ARCHIVE for resealed live archive checks")
class ResealedArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.archive = Path(os.environ["Q01_R06_ARCHIVE"])
        selection_path = Path(os.environ["Q01_R06_SELECTION"])
        selection_sha = os.environ["Q01_R06_SELECTION_SHA256"]
        cls.selection = load_selection(selection_path, selection_sha)
        cls.pins = selected_pins(cls.selection)
        verify(cls.archive, cls.pins, cls.selection)

    def reject_tamper(self, field, replacement, *, wrong_sha=False):
        with tempfile.TemporaryDirectory(prefix="q01-r06-release-view-",
                                         dir=self.archive.parent) as temporary:
            copied = Path(temporary) / "case"
            shutil.copytree(self.archive, copied, copy_function=os.link)
            case = copied / "selected-child-fault"
            installed_path = case / "installed-release.raw.json"
            installed = json.loads(installed_path.read_text())
            if field == "data_types":
                installed["node_contributions"][field] = replacement
            elif replacement is None:
                installed["assets"][0].pop(field)
            else:
                installed["assets"][0][field] = replacement
            replace_json(installed_path, installed)
            receipt_path = case / "package-receipt.raw.json"
            receipt = json.loads(receipt_path.read_text())
            if not wrong_sha:
                receipt["release_sha256"] = digest(installed_path)
                replace_json(receipt_path, receipt)
            manifest_path = copied / "evidence.sha256.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["selected-child-fault/installed-release.raw.json"] = digest(installed_path)
            if not wrong_sha:
                manifest["selected-child-fault/package-receipt.raw.json"] = digest(receipt_path)
            replace_json(manifest_path, manifest)
            with self.assertRaises(AssertionError):
                verify(copied, self.pins, self.selection)
            if not wrong_sha:
                self.assertFalse(release_views_equal(
                    json.loads((copied / "package-release.json").read_text()), installed))

    def test_resealed_semantic_tamper(self):
        for field, replacement in (
            ("required_libraries", ["libwrong.so"]),
            ("required_libraries", None),
            ("tested_ubuntu", ["22.04"]),
            ("tested_ubuntu", None),
            ("data_types", [{"id": "unexpected"}]),
        ):
            with self.subTest(field=field, replacement=replacement):
                self.reject_tamper(field, replacement)

    def test_wrong_receipt_sha_rejects_separately(self):
        self.reject_tamper("tested_ubuntu", ["22.04"], wrong_sha=True)


if __name__ == "__main__":
    unittest.main()
