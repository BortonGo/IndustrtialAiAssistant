"""Pure memory candidate bounds/provenance tests, independent of PostgreSQL and models."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'storage_tools'))
from conversation_memory import memory_candidates


class MemoryTests(unittest.TestCase):
    def state(self):
        pairs = [dict(questionId=str(i), position=i + 1, question=f'Сообщение {i}',
                      memoryText=f'Сообщение {i}', answer='Ответ с чужим именем: Иван') for i in range(12)]
        return pairs, dict(revision=0, items=[])

    def test_only_user_quotes_and_recent_six_turns_stay_outside_memory(self):
        pairs, memory = self.state()
        pairs[0].update(memoryText='Меня зовут Алексей. Проект работает без интернета.')
        plan = memory_candidates(pairs, memory, 8192)
        self.assertEqual(plan['compactThrough'], 4)
        self.assertTrue(all(int(c['questionId']) < 4 for c in plan['candidates']))
        self.assertIn('Меня зовут Алексей.', [c['text'] for c in plan['candidates']])
        self.assertNotIn('Иван', str(plan['candidates']))

    def test_transient_metrics_do_not_become_memory_and_short_history_needs_no_call(self):
        pairs, memory = self.state()
        pairs[0]['memoryText'] = 'CPU сейчас 40%. RAM 65%.'
        plan = memory_candidates(pairs, memory, 8192)
        self.assertNotIn('CPU', str(plan['candidates']))
        self.assertEqual(memory_candidates(pairs[:6], memory, 8192)['compactThrough'], 0)

    def test_never_advances_beyond_unoffered_large_message(self):
        pairs, memory = self.state()
        pairs[0]['memoryText'] = 'Большой исходный текст ' * 2000
        plan = memory_candidates(pairs, memory, 8192)
        self.assertEqual(plan['compactThrough'], 0)
        self.assertTrue(plan['memoryLimited'])

    def test_token_changes_with_source_and_revision(self):
        pairs, memory = self.state()
        original = memory_candidates(pairs, memory, 8192)['compactionToken']
        memory['revision'] = 1
        self.assertNotEqual(original, memory_candidates(pairs, memory, 8192)['compactionToken'])
        memory['revision'] = 0
        pairs[0]['memoryText'] = 'Исправленный факт'
        self.assertNotEqual(original, memory_candidates(pairs, memory, 8192)['compactionToken'])


if __name__ == '__main__':
    unittest.main()
