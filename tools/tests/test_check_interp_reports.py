import pathlib
import sys
import tempfile
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import check_interp_reports


class CheckInterpreterReportsTest(unittest.TestCase):
    def write(self, root, name, content):
        path = pathlib.Path(root) / name
        path.write_text(content, encoding="utf-8")
        return path

    def test_zero_reports_pass_without_policy(self):
        with tempfile.TemporaryDirectory() as root:
            misses = self.write(
                root,
                "misses.txt",
                "# psp-recomp interpreter misses v2\n",
            )
            profile = self.write(
                root,
                "profile.tsv",
                "# psp-recomp interpreter profile v1\n"
                "module\ttarget\tcalls\tinstructions\tsyscalls\t"
                "native_handoffs\tstep_limits\treturns\tfailures\n",
            )
            self.assertEqual(check_interp_reports.load_misses(misses), set())
            profiles = check_interp_reports.load_profiles(profile)
            self.assertEqual(profiles, {})
            self.assertEqual(
                check_interp_reports.validate(set(), profiles, {}),
                [],
            )

    def test_reviewed_budget_passes(self):
        with tempfile.TemporaryDirectory() as root:
            misses = self.write(root, "misses.txt", "overlay 0x08810000\n")
            profile = self.write(
                root,
                "profile.tsv",
                "overlay\t0x08810000\t2\t64\t1\t1\t0\t2\t0\n",
            )
            policy = self.write(
                root,
                "policy.txt",
                "overlay 0x08810000 2 64\n",
            )
            actual_misses = check_interp_reports.load_misses(misses)
            profiles = check_interp_reports.load_profiles(profile)
            self.assertEqual(
                check_interp_reports.validate(
                    actual_misses,
                    profiles,
                    check_interp_reports.load_policy(policy),
                ),
                [],
            )

    def test_unreviewed_or_exceeded_target_fails(self):
        profile = check_interp_reports.Profile(
            calls=3,
            instructions=65,
            syscalls=0,
            native_handoffs=0,
            step_limits=1,
            returns=2,
            failures=1,
        )
        key = ("overlay", 0x08810000)
        errors = check_interp_reports.validate(
            {key}, {key: profile}, {key: (2, 64)}
        )
        self.assertEqual(len(errors), 5)


if __name__ == "__main__":
    unittest.main()
