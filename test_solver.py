import functools
import itertools
import json
from pathlib import Path
import random
import select
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
BINARY = ROOT / 'dont-wordle'
FIXTURE = ['crane', 'gyppy', 'immix', 'fluff', 'wokka', 'qajaq', 'added',
           'hudud', 'boffo', 'allel', 'flaff', 'tooth', 'bubba', 'wootz',
           'huzza', 'adobo', 'abuzz', 'vivid', 'momma']


def feedback(guess, answer):
    result = ['.'] * 5
    unmatched = list(answer)
    for i in range(5):
        if guess[i] == answer[i]:
            result[i] = 'G'
            unmatched[i] = None
    for i in range(5):
        if result[i] == '.' and guess[i] in unmatched:
            result[i] = 'Y'
            unmatched[unmatched.index(guess[i])] = None
    return ''.join(result)


def exhaustive(words, answer, turns):
    @functools.lru_cache(None)
    def visit(remaining, used, depth):
        if not depth:
            return len(remaining) * (26 - len(used))
        scores = []
        for guess in remaining:
            if guess == answer:
                continue
            pattern = feedback(guess, answer)
            next_words = tuple(word for word in remaining if feedback(guess, word) == pattern)
            scores.append(visit(next_words, ''.join(sorted(set(used + guess))), depth - 1))
        return max(scores, default=-1)
    return visit(tuple(sorted(words)), '', turns)


def site_legal(word, history, answer):
    # Independent translation of the site's getCurrentWordErrorMessage.
    if not history:
        return True
    previous = history[-1]
    last = feedback(previous, answer)
    for i, tile in enumerate(last):
        if tile == 'G' and word[i] != previous[i]:
            return False
    for guess in history:
        tiles = feedback(guess, answer)
        for i, letter in enumerate(guess):
            if letter not in answer and letter in word:
                return False
            if tiles[i] in '.Y' and word[i] == letter:
                return False
    for letter in set(previous):
        minimum = sum(ch == letter and tile != '.' for ch, tile in zip(previous, last))
        if word.count(letter) < minimum:
            return False
    for guess in history:
        tiles = feedback(guess, answer)
        for letter in set(guess):
            positive = sum(ch == letter and tile != '.' for ch, tile in zip(guess, tiles))
            gray = any(ch == letter and tile == '.' for ch, tile in zip(guess, tiles))
            if positive and gray and word.count(letter) > positive:
                return False
    return True


class SolverTests(unittest.TestCase):
    def run_solver(self, words, answer, *args):
        with tempfile.TemporaryDirectory() as directory:
            dictionary = Path(directory) / 'words.txt'
            dictionary.write_text('\n'.join(words))
            return subprocess.run([str(BINARY), answer, '--dictionary', str(dictionary), '--json', *args],
                                  text=True, capture_output=True, timeout=30, cwd='/tmp')

    def check_path(self, result, words, answer):
        history = []
        used = set()
        for step in result['steps']:
            guess = step['guess']
            self.assertIn(guess, words)
            self.assertNotEqual(guess, answer)
            self.assertNotIn(guess, history)
            self.assertTrue(site_legal(guess, history, answer))
            history.append(guess)
            used.update(guess)
            remaining = [w for w in words if site_legal(w, history, answer)]
            self.assertEqual(step['feedback'], feedback(guess, answer))
            self.assertEqual(step['remaining_words'], len(remaining))
            self.assertEqual(step['unused_letters'], 26 - len(used))
            self.assertEqual(step['score'], len(remaining) * (26 - len(used)))
        self.assertEqual(result['score'], result['steps'][-1]['score'])

    def test_six_turn_global_optimum_not_greedy(self):
        expected = exhaustive(FIXTURE, 'crane', 6)
        self.assertEqual(expected, 10)
        process = self.run_solver(FIXTURE, 'crane')
        self.assertEqual(process.returncode, 0, process.stderr)
        result = json.loads(process.stdout)
        self.assertEqual(result['status'], 'OPTIMAL')
        self.assertEqual(result['score'], expected)
        self.assertEqual(len(result['steps']), 6)
        self.check_path(result, FIXTURE, 'crane')

    def test_small_dictionary_exhaustive_comparison(self):
        rng = random.Random(741)
        universe = [''.join(chars) for chars in itertools.product('abcde', repeat=5)]
        for _ in range(12):
            words = rng.sample(universe, 14)
            answer = words[0]
            process = self.run_solver(words, answer, '--turns', '3')
            result = json.loads(process.stdout)
            self.assertEqual(result['score'], exhaustive(words, answer, 3))
            if result['score'] >= 0:
                self.check_path(result, words, answer)
            else:
                self.assertEqual(process.returncode, 3)

    def test_duplicate_limits_match_site_dictionary(self):
        words = (ROOT / 'data/words.txt').read_text().split()
        for answer, history in [('apple', ['allee']), ('crane', ['qajaq']),
                                ('sissy', ['sessa']), ('crane', ['gyppy', 'immix'])]:
            process = self.run_solver(words, answer, '--history', ','.join(history), '--turns', str(len(history)))
            self.assertEqual(process.returncode, 0, process.stderr)
            self.check_path(json.loads(process.stdout), words, answer)

    def test_ruled_out_guess_and_answer_rejected(self):
        for history in ['gyppy,gyppy', 'crane']:
            process = self.run_solver(FIXTURE, 'crane', '--history', history)
            self.assertEqual(process.returncode, 1)

    def test_timeout_does_not_claim_optimality(self):
        process = self.run_solver(FIXTURE, 'crane', '--time-limit', '0.000000001')
        self.assertEqual(process.returncode, 2)
        self.assertNotEqual(json.loads(process.stdout)['status'], 'OPTIMAL')

    def test_ctrl_c_returns_best_complete_sequence(self):
        process = subprocess.Popen([str(BINARY), 'crane', '--json'], cwd='/tmp',
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            readable, _, _ = select.select([process.stderr], [], [], 15)
            self.assertTrue(readable, 'Solver did not produce an incumbent')
            incumbent = process.stderr.readline()
            self.assertIn('Best complete score:', incumbent)
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 2, stderr)
            result = json.loads(stdout)
            self.assertEqual(result['status'], 'BEST KNOWN')
            self.assertEqual(len(result['steps']), 6)
            self.assertGreaterEqual(result['score'], int(incumbent.split()[3]))
            words = (ROOT / 'data/words.txt').read_text().split()
            self.check_path(result, words, 'crane')
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()

    def test_no_surviving_sequence(self):
        process = self.run_solver(['apple', 'allee'], 'apple')
        self.assertEqual(process.returncode, 3)
        self.assertEqual(json.loads(process.stdout)['status'], 'NO SOLUTION')


if __name__ == '__main__':
    unittest.main()
