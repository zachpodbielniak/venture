#!/usr/bin/env python3
"""Pure fixture-ownership checks; never connects to a database."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("upgrade", Path(__file__).resolve().parents[1] / "tools/venture-upgrade-drill.py")
upgrade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upgrade)
URI = "postgres://fixture:private-password@127.0.0.1:55436/control"


class OwnershipTests(unittest.TestCase):
    def test_unowned_access_and_cleanup_do_not_connect(self):
        database = upgrade.PostgreSQL(URI)
        with patch.object(upgrade.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "unowned"):
                database.snapshot()
            database.remove()
            run.assert_not_called()

    def test_failed_creation_cannot_authorize_deletion(self):
        database = upgrade.PostgreSQL(URI)
        failure = subprocess.CompletedProcess([], 1, "", "database already exists")
        with patch.object(upgrade.subprocess, "run", return_value=failure) as run:
            with self.assertRaisesRegex(RuntimeError, "already exists"):
                database.create()
            self.assertFalse(database.owned)
            database.remove()
            self.assertEqual(run.call_count, 1)

    def test_clone_requires_this_service_and_owned_source(self):
        source, target = upgrade.PostgreSQL(URI), upgrade.PostgreSQL(URI)
        with patch.object(upgrade.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "owned stopped baseline"):
                target.create(source)
            source.owned = True
            source.service_uri = URI + "-different"
            with self.assertRaisesRegex(RuntimeError, "owned stopped baseline"):
                target.create(source)
            run.assert_not_called()

    def test_credentials_and_database_identity_are_explicit(self):
        with patch.dict(upgrade.os.environ, {"PGSERVICE": "personal", "PGDATABASE": "personal", "PGOPTIONS": "-c search_path=personal"}):
            source, target = upgrade.PostgreSQL(URI), upgrade.PostgreSQL(URI)
        self.assertNotEqual(source.name, target.name)
        success = subprocess.CompletedProcess([], 0, "", "")
        with patch.object(upgrade.subprocess, "run", return_value=success) as run:
            source.create()
            source.sql("SELECT 1")
            source.remove()
            create, query, remove = run.call_args_list
            self.assertEqual(create.kwargs["env"]["PGDATABASE"], "control")
            self.assertEqual(query.kwargs["env"]["PGDATABASE"], source.name)
            self.assertEqual(remove.kwargs["env"]["PGDATABASE"], "control")
            self.assertIn('DROP DATABASE "' + source.name + '"', remove.args[0])
            for call in run.call_args_list:
                self.assertNotIn("private-password", " ".join(call.args[0]))
                self.assertNotIn("PGSERVICE", call.kwargs["env"])
                self.assertEqual(call.kwargs["env"]["PGPASSFILE"], "/dev/null")
            self.assertFalse(source.owned)

    def test_connection_options_cannot_select_unowned_schema(self):
        with self.assertRaises(ValueError):
            upgrade.PostgreSQL(URI + "?options=-csearch_path=personal")


if __name__ == "__main__":
    unittest.main()
