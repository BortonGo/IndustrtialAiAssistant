"""Vision streaming/retry regressions; no GPU required."""
import io
import json
from email.message import Message
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "document_tools"))
from image_processing import ImageAnalyzer, RepeatedDescription, has_repeated_phrase


def event(value):
    return ("data: " + json.dumps(value, ensure_ascii=False) + "\r\n\r\n").encode()


def response(text, finish="stop", done=True, chunk_size=13):
    data = b": keepalive\n\n"
    for offset in range(0, len(text), chunk_size):
        data += event({"choices": [{"delta": {"content": text[offset:offset + chunk_size]}}]})
    if finish is not None:
        data += event({"choices": [{"delta": {}, "finish_reason": finish}]})
    data += event({"choices": [], "usage": {"completion_tokens": 50}})
    if done:
        data += b"data: [DONE]\n\n"
    stream = io.BytesIO(data)
    stream.headers = Message()
    stream.headers["Content-Type"] = "text/event-stream; charset=utf-8"
    return stream


class ImageStreamTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "build-qtcreator/test-artifacts/documents").mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT / "build-qtcreator/test-artifacts/documents")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.picture = self.work / "image.png"
        self.picture.write_bytes(b"fixture image bytes")
        self.progress = []
        self.analyzer = ImageAnalyzer("http://localhost/vision", "local-chat", "test", self.work / "cache",
                                      progress=self.progress.append, seed=42)
        self.opener = Mock()
        patcher = patch("image_processing.urllib.request.build_opener", return_value=self.opener)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_stop_stream_unicode_usage_and_cache(self):
        text = "Насос Ёж-42: давление 7,5 бар.\nКомната 218."
        stream = response(text)
        self.opener.open.return_value = stream
        diagnostics = {}
        self.assertEqual(self.analyzer.describe(self.picture, diagnostics=diagnostics), (text, False))
        self.assertTrue(stream.closed)
        self.assertEqual(diagnostics["attempts"][0]["usage"]["completion_tokens"], 50)
        self.assertEqual(self.analyzer.describe(self.picture), (text, False))
        self.assertEqual(self.opener.open.call_count, 1)
        payload = json.loads(self.opener.open.call_args.args[0].data)
        self.assertTrue(payload["stream"])
        self.assertEqual((payload["temperature"], payload["presence_penalty"]), (.7, 1.5))

    def test_repetition_closes_early_retries_once_and_caches_only_corrected_text(self):
        broken = response("Тип: схема. Надписи: " + "Место оператора ПК; " * 271, chunk_size=7)
        read = Mock(wraps=broken.readline)
        broken.readline = read
        self.opener.open.side_effect = [broken, response("Схема помещения, рабочее место оператора.")]
        diagnostics = {}
        expected = ("Схема помещения, рабочее место оператора.", False)
        self.assertEqual(self.analyzer.describe(self.picture, diagnostics=diagnostics), expected)
        self.assertTrue(broken.closed)
        self.assertLess(read.call_count, 150)  # Full old loop would require >1400 SSE lines.
        self.assertEqual([a["finish_reason"] for a in diagnostics["attempts"]], ["repetition", "stop"])
        self.assertEqual(len(self.progress), 1)
        payloads = [json.loads(c.args[0].data) for c in self.opener.open.call_args_list]
        self.assertNotEqual(payloads[0]["messages"], payloads[1]["messages"])
        self.assertEqual([p["seed"] for p in payloads], [42, 43])
        self.assertEqual(self.analyzer.describe(self.picture), expected)
        self.assertEqual(self.opener.open.call_count, 2)

    def test_second_loop_fails_without_cache_or_endless_retry(self):
        self.opener.open.side_effect = [response("Место оператора ПК; " * 80) for _ in range(2)]
        with self.assertRaisesRegex(RepeatedDescription, "повторной попытки"):
            self.analyzer.describe(self.picture)
        self.assertEqual(self.opener.open.call_count, 2)
        self.assertEqual(list((self.work / "cache").glob("*.json")), [])

    def test_legitimate_repeated_labels_and_complete_table_survive_unchanged(self):
        examples = [
            "| Устройство | Давление |\n|---|---|\n" + "| Насос | 7.5 |\n" * 50,
            "Место оператора ПК\n" * 30,
            "\t".join(["Место оператора ПК"] * 30),
            "Давление насоса 7.5 бар; " * 30,
            "Место оператора ПК; " * 5,
            " ".join(f"Место оператора ПК {i}: давление 7.5 бар;" for i in range(30)),
        ]
        for index, text in enumerate(examples):
            with self.subTest(index=index):
                self.assertFalse(has_repeated_phrase(text))
                self.analyzer.profile = str(index)
                self.opener.open.return_value = response(text)
                self.assertEqual(self.analyzer.describe(self.picture), (text, False))
        self.assertEqual(self.opener.open.call_count, len(examples))

    def test_truncated_description_is_reported_and_not_cached_or_retried(self):
        self.opener.open.side_effect = [response("Часть большой таблицы", "length") for _ in range(2)]
        for _ in range(2):
            self.assertEqual(self.analyzer.describe(self.picture), ("Часть большой таблицы", True))
        self.assertEqual(self.opener.open.call_count, 2)
        self.assertFalse((self.work / "cache").exists())

    def test_incomplete_empty_or_error_stream_is_never_cached(self):
        streams = [response("Часть ответа", done=False), response("Текст", finish=None), response(""),
                   response("Текст", finish="content_filter")]
        error = response("ignored")
        error.seek(0)
        error.truncate()
        error.write(event({"error": {"message": "model failed"}}))
        error.seek(0)
        streams.append(error)
        for stream in streams:
            with self.subTest(stream=stream):
                self.opener.open.return_value = stream
                with self.assertRaises(ValueError):
                    self.analyzer.describe(self.picture)
                self.assertTrue(stream.closed)
                self.assertFalse((self.work / "cache").exists())
        self.assertEqual(self.opener.open.call_count, len(streams))

    def test_deadline_also_limits_retry(self):
        self.opener.open.return_value = response("Место оператора ПК; " * 80)
        # Time budget is shared by both attempts, not reset on retry.
        clock = Mock(return_value=0)
        def expire(message):
            clock.return_value = 301
        self.analyzer.progress = expire
        with patch("image_processing.time.monotonic", clock):
            with self.assertRaises(TimeoutError):
                self.analyzer.describe(self.picture)
        self.assertEqual(self.opener.open.call_count, 1)


if __name__ == "__main__":
    unittest.main()
