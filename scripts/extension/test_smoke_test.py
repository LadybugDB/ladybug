import contextlib
import io
import unittest
import urllib.error
from unittest import mock

import smoke_test
from smoke_test import baseline_unreachable, counts_as_failure, engines_for_line, missing_from, validate_names

LOAD_ERROR = "exit code 1: Error: libfts.lbug_extension: undefined symbol: _ZN4lbug6common11ValueVector10setAllNullEv."
NOT_SERVED = 'exit code 1: RuntimeError: IO exception: HTTP Returns: 404, Failed to download extension: "fts" from x.'
UNREACHABLE = "exit code 1: RuntimeError: IO exception: Failed to download extension: fts at URL x (ERROR: Read)"


class CountsAsFailureTest(unittest.TestCase):
    def test_a_check_that_loads_never_counts(self):
        self.assertFalse(counts_as_failure(None, None))

    def test_a_failure_counts_without_a_baseline_or_when_the_baseline_loads(self):
        self.assertTrue(counts_as_failure(LOAD_ERROR, None))

    def test_a_failure_the_baseline_shares_does_not_count(self):
        self.assertFalse(counts_as_failure(LOAD_ERROR, LOAD_ERROR))

    def test_a_failure_does_not_count_when_the_baseline_does_not_serve_the_extension(self):
        self.assertFalse(counts_as_failure(LOAD_ERROR, NOT_SERVED))

    def test_an_unreachable_baseline_cannot_hide_a_failure(self):
        self.assertTrue(counts_as_failure(LOAD_ERROR, UNREACHABLE))
        self.assertTrue(counts_as_failure(LOAD_ERROR, "timed out after 300 s"))

    def test_baseline_unreachable(self):
        self.assertTrue(baseline_unreachable(UNREACHABLE))
        self.assertFalse(baseline_unreachable(NOT_SERVED))
        self.assertFalse(baseline_unreachable(LOAD_ERROR))


class BaselineCheckTest(unittest.TestCase):
    def run_baseline_check(self, results):
        with mock.patch.object(smoke_test, "check", side_effect=results) as check:
            return smoke_test.baseline_check("python", "https://cdn/", "fts", "work"), check.call_count

    def test_an_unreachable_baseline_is_tried_once_more(self):
        self.assertEqual(self.run_baseline_check([UNREACHABLE, None]), (None, 2))
        self.assertEqual(self.run_baseline_check([UNREACHABLE, UNREACHABLE]), (UNREACHABLE, 2))

    def test_a_load_failure_is_not_retried(self):
        self.assertEqual(self.run_baseline_check([LOAD_ERROR]), (LOAD_ERROR, 1))


class ListExtensionsTest(unittest.TestCase):
    def list_with_error(self, code):
        error = urllib.error.HTTPError("https://cdn/v0.22.0/linux_amd64/", code, "error", {}, None)
        with mock.patch.object(smoke_test, "list_dirs", side_effect=error):
            return smoke_test.list_extensions("https://cdn/v0.22.0/linux_amd64/")

    def test_a_line_the_repository_does_not_serve_has_no_extensions(self):
        self.assertEqual(self.list_with_error(404), [])

    def test_any_other_listing_error_stops_the_run(self):
        with self.assertRaises(urllib.error.HTTPError):
            self.list_with_error(503)

    def test_common_is_not_an_extension(self):
        with mock.patch.object(smoke_test, "list_dirs", return_value=["common", "fts", "json"]):
            self.assertEqual(smoke_test.list_extensions("https://cdn/"), ["fts", "json"])


class MainTest(unittest.TestCase):
    def run_main(self, *args):
        output = io.StringIO()
        with mock.patch("sys.argv", ["smoke_test.py", *args]), contextlib.redirect_stdout(output), \
                mock.patch.object(smoke_test.tempfile, "mkdtemp", return_value="work"):
            return smoke_test.main(), output.getvalue()

    def test_non_release_lines_skip_before_any_network_call(self):
        with mock.patch.object(smoke_test, "released_versions", side_effect=AssertionError("network")):
            self.assertEqual(self.run_main("--line", "vdev")[0], 0)

    @staticmethod
    def listing(url):
        return ["fts"] if url.startswith("https://new/") else ["fts", "json"]

    def test_a_dropped_extension_fails_the_run(self):
        # Every check loads, but the baseline serves json and the tested tree does not.
        with mock.patch.object(smoke_test, "released_versions", return_value=["0.21.0"]), \
                mock.patch.object(smoke_test, "extension_line", return_value="v0.21.0"), \
                mock.patch.object(smoke_test, "make_venv", return_value="python"), \
                mock.patch.object(smoke_test, "check", return_value=None), \
                mock.patch.object(smoke_test, "list_extensions", side_effect=self.listing):
            result, output = self.run_main("--line", "v0.21.0", "--repo", "https://new/", "--baseline", "https://cdn/")
        self.assertEqual(result, 1)
        self.assertRegex(output, r"json +FAILED: the baseline serves it")


class MissingFromTest(unittest.TestCase):
    def test_an_extension_the_published_tree_serves_must_stay(self):
        self.assertEqual(missing_from(["fts"], ["fts", "json", "vector"]), ["json", "vector"])

    def test_nothing_is_missing_when_the_line_is_not_published_yet(self):
        self.assertEqual(missing_from(["fts"], []), [])
        self.assertEqual(missing_from([], []), [])


class InputsTest(unittest.TestCase):
    def test_non_release_lines_have_no_engines(self):
        self.assertEqual(engines_for_line("vdev", ["0.22.0"]), [])

    def test_repository_urls_cannot_break_out_of_the_install_statement(self):
        self.assertEqual(smoke_test.resolve("https://extension.ladybugdb.com"), "https://extension.ladybugdb.com/")
        for url in ("https://cdn/' x", "https://cdn/\\x", "https://cdn/ x", "file:///etc/"):
            with self.assertRaises(SystemExit):
                smoke_test.resolve(url)

    def test_extension_names_must_be_identifiers(self):
        self.assertEqual(validate_names(["fts", "unity_catalog"]), ["fts", "unity_catalog"])
        with self.assertRaises(SystemExit):
            validate_names(["fts' FROM 'http://evil/"])


if __name__ == "__main__":
    unittest.main()
