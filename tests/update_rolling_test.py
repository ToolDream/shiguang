"""Offline rolling publication fault cases; no gh or remote writes."""
import json
import sys
from zipfile import ZipFile
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from update_rolling import publish, FIXED, BACKUP, STAGED
from update_publish import DEV_TAG, LEGACY, NOTE, Rolling, formal
from update_publish import legacy_record, select_development, verify_legacy_asset
from types import SimpleNamespace
from tempfile import TemporaryDirectory
from unittest.mock import patch

class Fake:
    def __init__(self):
        self.release_id = 147
        self.release_tag = "dev-latest"
        self.main = None
        self.ref = None
        self.current = None
        self.pending = None
        self.retired = None
        self.assets = {}
        self.fail_after = None

    def confirm_current(self):
        assert self.assets[FIXED] == self.current["digest"]

    def main_head(self):
        return self.main

    def set_notes(self, current, pending, retired=None):
        self.current, self.pending, self.retired = current, pending, retired
        if current and not current.get("legacy"): self.release_tag = "dev-latest"

    def upload(self, name, asset):
        assert name not in self.assets
        self.assets[name] = asset["digest"]

    def rename(self, old, new):
        assert new not in self.assets
        self.assets[new] = self.assets.pop(old)
        if self.fail_after == old:
            self.fail_after = None
            raise IOError("simulated reply loss")

    def delete(self, name):
        self.assets.pop(name)

    def move_ref(self, commit):
        self.ref = commit

def build(n):
    return {"commit": str(n)*40, "order": n, "digest": str(n)*64}

class RollingTests(unittest.TestCase):
    def test_two_builds_same_id_late_old_and_idempotent_rebuild(self):
        r = Fake()
        for n in (1,2):
            r.main = build(n)["commit"]
            publish(r, build(n))
            self.assertEqual(r.release_id,147)
            self.assertEqual(r.assets,{FIXED:build(n)["digest"]})
        publish(r, dict(build(2), digest="newly resealed bytes"))
        with self.assertRaises(ValueError):
            publish(r, build(1))
        self.assertEqual(r.assets,{FIXED:build(2)["digest"]})

    def test_reply_loss_after_detaching_old_asset_is_resumable(self):
        r = Fake()
        r.main = build(1)["commit"]
        publish(r,build(1))
        r.main = build(2)["commit"]
        r.fail_after = FIXED
        with self.assertRaises(IOError):
            publish(r,build(2))
        self.assertEqual(r.assets,{BACKUP:build(1)["digest"],STAGED:build(2)["digest"]})
        publish(r,build(2))
        self.assertEqual(r.assets,{FIXED:build(2)["digest"]})

    def test_new_main_recovers_a_stale_pending_switch_without_ref_rollback(self):
        r = Fake()
        r.main = build(1)["commit"]
        publish(r, build(1))
        r.main = build(2)["commit"]
        r.fail_after = FIXED
        with self.assertRaises(IOError):
            publish(r, build(2))
        r.main = build(3)["commit"]
        with self.assertRaises(ValueError):
            publish(r, build(2))
        self.assertEqual(r.assets[BACKUP], build(1)["digest"])
        publish(r, build(3))
        self.assertEqual(r.ref, build(3)["commit"])
        self.assertEqual(r.assets, {FIXED: build(3)["digest"]})

    def test_main_advances_during_staging_without_disturbing_fixed_asset(self):
        class AdvancingMain(Fake):
            def upload(self, name, asset):
                super().upload(name, asset)
                if asset["commit"] == build(2)["commit"]:
                    self.main = build(3)["commit"]
        r = AdvancingMain()
        r.main = build(1)["commit"]
        publish(r, build(1))
        r.main = build(2)["commit"]
        with self.assertRaises(ValueError):
            publish(r, build(2))
        self.assertEqual(r.assets, {FIXED:build(1)["digest"], STAGED:build(2)["digest"]})
        self.assertEqual(r.ref, build(1)["commit"])
        publish(r, build(3))
        self.assertEqual(r.assets, {FIXED:build(3)["digest"]})

    def test_legacy_bootstrap_reuses_id_and_keeps_old_tag_after_lost_reply(self):
        r = Fake()
        r.current = dict(build(1), legacy=True)
        r.legacy_tag = "dev-111111111111"
        r.release_tag = r.legacy_tag
        r.assets[FIXED] = build(1)["digest"]
        r.main = build(2)["commit"]
        r.fail_after = FIXED
        with self.assertRaises(IOError):
            publish(r, build(2))
        self.assertEqual(r.release_tag, r.legacy_tag)
        self.assertEqual(r.assets[BACKUP], build(1)["digest"])
        publish(r, build(2))
        self.assertEqual(r.release_tag, "dev-latest")
        self.assertEqual(r.release_id, 147)
        self.assertEqual(r.legacy_tag, "dev-111111111111")
        self.assertEqual(r.assets, {FIXED:build(2)["digest"]})

    def test_legacy_ambiguity_or_formal_release_cannot_be_adopted(self):
        stable = dict(id=1, tag_name="v1.2.3", prerelease=False)
        legacy = dict(id=147, tag_name="dev-" + "1"*12, prerelease=True)
        self.assertIsNone(select_development([stable]))
        self.assertEqual(select_development([stable, legacy]), legacy)
        with self.assertRaises(ValueError):
            select_development([legacy, dict(legacy, id=148, tag_name="dev-" + "2"*12)])
        with self.assertRaises(ValueError):
            select_development([dict(legacy, prerelease=False)])

    def test_unknown_asset_or_ref_never_gets_replaced(self):
        r = Fake()
        r.main = build(1)["commit"]
        r.assets[FIXED] = "unknown"
        with self.assertRaises(ValueError):
            publish(r, build(1))
        self.assertEqual(r.assets[FIXED], "unknown")

    def test_commit_readback_loss_does_not_lose_previous_bytes(self):
        class LostReply(Fake):
            def set_notes(self, current, pending, retired=None):
                super().set_notes(current, pending, retired)
                if retired:
                    raise IOError("commit reply lost")
        r = LostReply()
        r.main = build(1)["commit"]
        publish(r, build(1))
        r.main = build(2)["commit"]
        with self.assertRaises(IOError):
            publish(r, build(2))
        self.assertEqual(r.assets, {FIXED:build(2)["digest"], BACKUP:build(1)["digest"]})
        # Simulate a fresh invocation after the no-op readback of the same build.
        r.set_notes = lambda current, pending, retired=None: Fake.set_notes(r, current, pending, retired)
        publish(r, build(2))
        self.assertEqual(r.assets, {FIXED:build(2)["digest"]})

class MigrationAdapterTests(unittest.TestCase):
    @patch.dict("os.environ", {"RUN_URL": "https://example.invalid/run"})
    @patch("update_publish.api")
    def test_cutover_updates_exact_legacy_release_id_without_a_create(self, api):
        r = Rolling.__new__(Rolling)
        old = dict(build(1), legacy=True, note=LEGACY + "{} -->", tag="dev-111111111111")
        new = dict(build(2), note=NOTE + "signed-data -->")
        r.item = {"id": 454381302, "tag_name": old["tag"]}
        r.legacy_body = old["note"]
        r.current = old
        r.ref = None
        def endpoint(path, verb, fields):
            self.assertEqual((path, verb), ("releases/454381302", "PATCH"))
            return dict(id=454381302, body=fields["body"], prerelease=True, draft=False,
                        tag_name=fields.get("tag_name", old["tag"]))
        api.side_effect = endpoint
        r.set_notes(old, new)
        self.assertNotIn("tag_name", api.call_args.args[2])
        r.ref = new["commit"]
        r.set_notes(new, None, old)
        self.assertEqual(api.call_args.args[2]["tag_name"], DEV_TAG)
        self.assertEqual(r.item["id"], 454381302)

    @patch("update_publish.scan_releases", return_value=[dict(
        id=454381302, tag_name="dev-111111111111", prerelease=True, draft=False, body="unknown")])
    @patch("update_publish.tag_ref", return_value=None)
    @patch("update_publish.api")
    def test_unknown_legacy_machine_record_refuses_before_writes(self, api, tag_ref, releases):
        with TemporaryDirectory() as directory, self.assertRaises(ValueError):
            Rolling("public", Path(directory))
        api.assert_not_called()


    @patch("update_publish.tag_ref")
    @patch("update_publish.subprocess.check_output", return_value=b"1\n")
    def test_legacy_requires_exact_repo_tag_ref_and_archive_identity(self, order, tag):
        commit = build(1)["commit"]
        tag.return_value = {"type": "commit", "sha": commit}
        metadata = dict(schema=1, repo="ToolDream/shiguang", mode="dev", tag="dev-" + commit[:12],
                        commit=commit, ref_object=commit, zip_sha256=build(1)["digest"])
        note = LEGACY + json.dumps(metadata) + " -->"
        owned = legacy_record(note, metadata["tag"])
        with TemporaryDirectory() as d:
            zip_path = Path(d) / FIXED
            manifest = dict(repo=metadata["repo"], ref="refs/heads/main", commit=commit, ref_object=commit)
            with ZipFile(zip_path, "w") as z:
                z.writestr("BUILD-MANIFEST.json", json.dumps(manifest))
            verify_legacy_asset(zip_path, owned)
            with ZipFile(zip_path, "w") as z:
                z.writestr("BUILD-MANIFEST.json", json.dumps(dict(manifest, commit=build(2)["commit"])))
            with self.assertRaises(ValueError):
                verify_legacy_asset(zip_path, owned)
        tag.return_value = {"type": "commit", "sha": build(2)["commit"]}
        with self.assertRaises(ValueError):
            legacy_record(note, metadata["tag"])


class ImmutableFormalTests(unittest.TestCase):
    @patch("update_publish.verify_formal_ref")
    @patch("update_publish.releases")
    @patch("update_publish.read_assets")
    @patch("update_publish.record")
    @patch("update_publish.download")
    @patch("update_publish.verify_final")
    @patch("update_publish.payload_matches")
    @patch("update_publish.api")
    @patch("update_publish.gh")
    def test_formal_existing_same_payload_is_readonly_and_other_payload_is_refused(
            self, gh, api, payload, verify, download, signed_record, assets, releases, verify_ref):
        src = build(2)
        src["note"] = "<!-- shiguang-update-v2 signed-example -->"
        a = SimpleNamespace(version="v1.2.3", ref_object="abc", public="key",
                            commit=src["commit"], raw=Path("unused.zip"))
        releases.return_value = dict(id=20, prerelease=False, draft=False,
            body="<!-- shiguang-formal-ref abc -->\n" + src["note"])
        assets.return_value = {FIXED: {"id": 21, "state": "uploaded"}}
        signed_record.return_value = src
        download.return_value = src["digest"]
        with TemporaryDirectory() as work:
            payload.return_value = True
            formal(a, src, Path(work))
            payload.return_value = False
            with self.assertRaises(ValueError):
                formal(a, src, Path(work))
        self.assertFalse(api.called)
        self.assertFalse(gh.called)

if __name__ == "__main__":
    unittest.main()
