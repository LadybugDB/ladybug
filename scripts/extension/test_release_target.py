import unittest

from release_target import release_target

RELEASED_021 = ["v0.20.4", "v0.21.0", "v0.21.1", "v0.21.2"]


class ReleaseTargetTest(unittest.TestCase):
    def test_nightly_writes_vdev(self):
        self.assertEqual(release_target(True, "tag", "v0.21.2", "0.21.0", RELEASED_021), "vdev")
        self.assertEqual(release_target(True, "branch", "main", "0.21.0", RELEASED_021), "vdev")

    def test_branch_cannot_rewrite_a_released_line(self):
        # The 16 October run: a branch whose CMakeLists.txt still named the released 0.21 line.
        with self.assertRaises(SystemExit):
            release_target(False, "branch", "sync_submodule2", "0.21.0", RELEASED_021)

    def test_tag_of_the_line_may_rebuild_it(self):
        self.assertEqual(release_target(False, "tag", "v0.21.2", "0.21.0", RELEASED_021), "v0.21.0")
        self.assertEqual(release_target(False, "tag", "v0.18.3", "0.18.1", ["v0.18.0", "v0.18.3"]), "v0.18.1")

    def test_tag_of_another_minor_cannot_rewrite_a_released_line(self):
        with self.assertRaises(SystemExit):
            release_target(False, "tag", "v0.22.0", "0.21.0", RELEASED_021 + ["v0.22.0"])

    def test_unreleased_line_accepts_any_ref(self):
        self.assertEqual(release_target(False, "branch", "main", "0.22.0", RELEASED_021), "v0.22.0")

    def test_pre_release_tags_neither_release_a_line_nor_rebuild_it(self):
        self.assertEqual(release_target(False, "branch", "main", "0.23.0", ["v0.23.0-rc1"]), "v0.23.0")
        with self.assertRaises(SystemExit):
            release_target(False, "tag", "v0.23.0-rc1", "0.23.0", ["v0.23.0", "v0.23.0-rc1"])

    def test_rejects_an_unexpected_version_string(self):
        for version in ("0.22.0; rm -rf /", "dev", "0.22"):
            with self.assertRaises(SystemExit):
                release_target(False, "tag", "v0.22.0", version, ["v0.22.0"])


if __name__ == "__main__":
    unittest.main()
