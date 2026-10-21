"""Benchmark guardrails; these fast tests do not launch a browser or time code."""
from contextlib import redirect_stdout
from copy import deepcopy
import io
import unittest
from render_benchmark import compare_results, css_from_source


class RenderBenchmarkGuardTests(unittest.TestCase):
    def setUp(self):
        self.baseline = {'workload': 'mixed-chat-v1', 'messages': 2000,
                         'environment': {'browser': 'fixed'},
                         'metrics': {'render_ms': {'median': 100}}}

    def compare(self, result, **kwargs):
        with redirect_stdout(io.StringIO()):
            compare_results(result, self.baseline, **kwargs)

    def test_regressions_are_advisory_unless_enforced(self):
        result = deepcopy(self.baseline)
        result['metrics']['render_ms']['median'] = 130
        self.compare(result)
        with self.assertRaisesRegex(SystemExit, 'Regression threshold exceeded'):
            self.compare(result, enforce=True)
        self.compare(result, enforce=True, threshold=35)

    def test_mismatched_environment_cannot_be_enforced(self):
        result = deepcopy(self.baseline)
        result['environment']['browser'] = 'other'
        self.compare(result)
        with self.assertRaisesRegex(SystemExit, 'different environments'):
            self.compare(result, enforce=True)

    def test_wrong_workload_and_invalid_baseline_fail(self):
        result = deepcopy(self.baseline)
        result['messages'] = 10
        with self.assertRaisesRegex(SystemExit, 'Incompatible baseline'):
            self.compare(result)
        self.baseline['metrics']['render_ms']['median'] = 0
        with self.assertRaisesRegex(SystemExit, 'Invalid baseline median'):
            self.compare(self.baseline)

    def test_current_application_css_is_extracted(self):
        css = css_from_source()
        self.assertIn('*{margin:0;padding:0}', css)
        self.assertIn('.enil-sticon-inline', css)
        self.assertIn('.enil-bubble', css)
        self.assertNotIn('stringByAppendingString', css)


if __name__ == '__main__':
    unittest.main()
