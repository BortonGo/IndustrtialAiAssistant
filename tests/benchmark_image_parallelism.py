"""Opt-in local vision benchmark. Own ports/processes; no app DB or description cache."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import configparser
import json
from pathlib import Path
import socket
import statistics
import subprocess
import sys
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "document_tools"))
from image_processing import ImageAnalyzer, PROMPT, PROMPT_VERSION, VISION_SAMPLING
import read_pdf

HIDDEN = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def http(url, payload=None, timeout=300):
    request = urllib.request.Request(url, data=None if payload is None else json.dumps(payload).encode(),
                                     headers={"Content-Type": "application/json"})
    with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=timeout) as response:
        return json.load(response)


def emit(message):
    print(message, flush=True)


class Server:
    def __init__(self, executable, arguments, port, log):
        self.port, self.process, self.log = port, None, None
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", port))  # Never reuse or stop an unrelated server.
        self.log = log.open("w", encoding="utf-8")
        self.process = subprocess.Popen([str(executable), *arguments, "--host", "127.0.0.1", "--port", str(port)],
                                        stdout=self.log, stderr=subprocess.STDOUT, creationflags=HIDDEN)

    def __enter__(self):
        try:
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError(f"Server {self.port} exited with {self.process.returncode}")
                try:
                    if http(f"http://127.0.0.1:{self.port}/health", timeout=2).get("status") == "ok":
                        return self
                except (OSError, ValueError):
                    pass
                time.sleep(.25)
            raise TimeoutError(f"Server {self.port} did not become ready")
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *args):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        if self.log:
            self.log.close()


def describe(analyzer, item):
    start = time.perf_counter()
    diagnostics, error, text, truncated = {}, None, "", False
    try:
        text, truncated = analyzer.describe(item["path"], diagnostics=diagnostics)
    except Exception as exception:
        error = str(exception)
    if diagnostics.get("cacheHit"):
        raise RuntimeError("Benchmark unexpectedly used a cached description")
    attempts = diagnostics.get("attempts", [])
    final = attempts[-1] if attempts else {}
    return dict(item, seconds=time.perf_counter() - start, text=text, error=error,
                finish_reason="error" if error else "length" if truncated else "stop",
                usage=final.get("usage", {}), timings=final.get("timings", {}), attempts=attempts)


def gpu_samples(stop, samples):
    while not stop.is_set():
        try:
            output = subprocess.check_output(["nvidia-smi", "--query-gpu=memory.used,utilization.gpu",
                                              "--format=csv,noheader,nounits"], creationflags=HIDDEN, timeout=5)
            memory, load = map(int, output.decode().splitlines()[0].split(","))
            samples.append({"memoryMiB": memory, "gpuPercent": load})
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
        stop.wait(2)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pdf", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--count", type=int, default=20)
    parser.add_argument("--modes", type=int, nargs="+", choices=(1, 2), default=[1, 2])
    parser.add_argument("--port", type=int, default=8096)
    args = parser.parse_args()
    if not 2 <= args.count <= 100:
        parser.error("count must be between 2 and 100")
    if args.output.exists() and any(args.output.iterdir()):
        parser.error("Use an empty output directory so earlier images/results cannot affect this run")
    args.output.mkdir(parents=True, exist_ok=True)
    settings = configparser.ConfigParser()
    settings.read(ROOT / "local-assistant.ini", encoding="utf-8-sig")
    model = settings["models"]
    executable = ROOT / "tools/llama-cpp/cuda/llama-server.exe"
    warnings = []
    manifest_path = args.output / "images.json"
    blocks = read_pdf.extract_blocks(args.pdf, args.output / "images", ROOT / "tools/poppler-26.09.0/Library/bin",
                                    warnings, emit)
    unique = list({b["imagePath"]: {"path": b["imagePath"], "page": b["pageNumber"]}
                   for b in blocks if b["type"] == "image"}.values())
    count = min(args.count, len(unique))
    if count < 2:
        raise ValueError("Need at least two unique images")
    images = [unique[round(i * (len(unique) - 1) / (count - 1))] for i in range(count)]
    manifest_path.write_text(json.dumps(images, ensure_ascii=False, indent=2), encoding="utf-8")
    report = {"source": str(args.pdf), "settings": dict(model), "promptVersion": PROMPT_VERSION, "prompt": PROMPT,
              "sampling": VISION_SAMPLING, "images": len(images), "warnings": warnings, "runs": [],
              "hardware": subprocess.check_output(["nvidia-smi", "--query-gpu=name,memory.total,driver_version",
                  "--format=csv,noheader"], creationflags=HIDDEN).decode().strip()}
    embedding_args = ["--model", str(ROOT / model["embeddingModel"]), "--embedding", "--alias", "embeddinggemma",
                      "--ctx-size", "4096", "--parallel", "2", "--batch-size", "2048", "--ubatch-size", "2048",
                      "--gpu-layers", "99"]
    emit(f"Benchmark: {len(images)} unique images, modes {args.modes}; separate ports {args.port}/{args.port + 1}")
    with Server(executable, embedding_args, args.port + 1, args.output / "embedding.log"):
        for run_number, workers in enumerate(args.modes, 1):
            name = f"run-{run_number}-parallel-{workers}"
            seed = 42 + (run_number - 1) // 2
            ctx = model.get("chatContextSize", "16384")
            server_args = ["--model", str(ROOT / model["chatModel"]), "--mmproj", str(ROOT / model["chatMmproj"]),
                           "--alias", "local-chat", "--ctx-size", ctx, "--parallel", str(workers),
                           "--gpu-layers", model.get("chatGpuLayers", "20"), "--jinja", "--reasoning", "off",
                           "--cache-ram", "0", "--cache-type-k", model.get("chatKvCacheType", "f16"),
                           "--cache-type-v", model.get("chatKvCacheType", "f16"), "--image-max-tokens", "1024"]
            if workers > 1:
                server_args += ["--kv-unified", "--kv-unified-per-slot", ctx]
            if model.get("chatKvCacheType", "f16") == "q8_0":
                server_args += ["--flash-attn", "on"]
            if not model.getboolean("mmprojGpu", True):
                server_args += ["--no-mmproj-offload"]
            emit(f"{name}: starting")
            with Server(executable, server_args, args.port, args.output / f"{name}.log"):
                slots = http(f"http://127.0.0.1:{args.port}/slots")
                if any(slot["n_ctx"] != int(ctx) for slot in slots):
                    raise RuntimeError(f"Unexpected slot context: {slots}")
                emit(f"{name}: slots={len(slots)}, context={slots[0]['n_ctx']}; warm-up")
                analyzer = ImageAnalyzer(f"http://127.0.0.1:{args.port}/v1/chat/completions", "local-chat",
                                         name, args.output / name / "cache", progress=emit, seed=seed)
                warmup = describe(analyzer, {"path": str(ROOT / "test-documents/images/standalone-plate.png"), "page": 0})
                if warmup["error"]:
                    raise RuntimeError("Warmup failed: " + warmup["error"])
                stop, samples = threading.Event(), []
                monitor = threading.Thread(target=gpu_samples, args=(stop, samples), daemon=True)
                monitor.start()
                started = time.perf_counter()
                results = []
                try:
                    with ThreadPoolExecutor(max_workers=workers) as pool:
                        futures = [pool.submit(describe, analyzer, item) for item in images]
                        for future in as_completed(futures):
                            result = future.result()
                            results.append(result)
                            with (args.output / f"{name}.jsonl").open("a", encoding="utf-8") as trace:
                                trace.write(json.dumps(result, ensure_ascii=False) + "\n")
                            emit(f"{name}: {len(results)}/{len(images)}, page {result['page']}, "
                                 f"{result['seconds']:.1f}s, {result['usage'].get('completion_tokens')} tokens, "
                                 f"attempts={len(result['attempts'])}, error={result['error']}")
                finally:
                    elapsed = time.perf_counter() - started
                    stop.set()
                    monitor.join(timeout=7)
                run = {"workers": workers, "seed": seed, "seconds": elapsed, "imagesPerMinute": len(images) * 60 / elapsed,
                       "peakGpuMemoryMiB": max((x["memoryMiB"] for x in samples), default=None),
                       "meanGpuPercent": statistics.mean(x["gpuPercent"] for x in samples) if samples else None,
                       "totalOutputTokens": sum(x["usage"].get("completion_tokens", 0) for x in results),
                       "truncated": sum(x["finish_reason"] == "length" for x in results),
                       "errors": sum(x["error"] is not None for x in results),
                       "retries": sum(max(0, len(x["attempts"]) - 1) for x in results),
                       "results": results, "samples": samples, "serverArguments": server_args}
                report["runs"].append(run)
                (args.output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
                emit(f"{name}: DONE {elapsed:.1f}s, {run['imagesPerMinute']:.2f} images/min, "
                     f"peak GPU {run['peakGpuMemoryMiB']} MiB")
    emit("DONE: all benchmark servers stopped")


if __name__ == "__main__":
    main()
