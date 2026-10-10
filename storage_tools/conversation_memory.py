"""Bounded extractive memory: only original user quotes, never invented summaries."""
import hashlib
import json
import re


def memory_candidates(pairs, memory, context_size):
    candidates = list(memory['items'])
    through = 0
    # Keep the most recent six complete turns verbatim. Compression is incremental.
    total = sum(len(p['question']) + len(p['answer']) for p in pairs)
    needed = len(pairs) > 10 or (len(pairs) > 6 and total > min(12000, context_size))
    if needed:
        for pair in pairs[:-6][:4]:
            fragments = []
            for line in re.split(r'(?<=[.!?])\s+|\n+', pair['memoryText']):
                line = line.strip()
                if not line:
                    continue
                # Do not retain transient machine telemetry, including pasted values.
                if re.search(r'(?i)(cpu|ram|цп|озу|процессор|оперативн).*\d[\d.,]*\s*%', line):
                    continue
                for start in range(0, len(line), 400):
                    fragments.append(dict(questionId=pair['questionId'], text=line[start:start + 400]))
            proposed = candidates + fragments
            if len(proposed) > 48 or len(json.dumps(proposed, ensure_ascii=False).encode('utf-8')) > 9000:
                break  # Never advance past text that was not offered to the model.
            candidates = proposed
            through = pair['position']
    payload = dict(revision=memory['revision'], through=through, candidates=candidates)
    token = hashlib.sha256(json.dumps(payload, ensure_ascii=False, sort_keys=True).encode('utf-8')).hexdigest()
    return dict(candidates=candidates, compactThrough=through, compactionToken=token,
                memoryLimited=needed and not through)
