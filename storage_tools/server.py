"""Private JSON-lines process protocol. stdout contains replies only."""
import argparse
import json
from pathlib import Path
import sys
import psycopg
from runtime import LocalPostgres
from repository import Repository


def output(value):
    print(json.dumps(value, ensure_ascii=False, default=str, allow_nan=False), flush=True)


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--data', required=True, type=Path)
    parser.add_argument('--model', required=True, type=Path)
    parser.add_argument('--port', type=int, default=55432)
    args = parser.parse_args()
    runtime = LocalPostgres(args.root, args.data, args.port)
    repository = None
    try:
        options = runtime.start()
        repository = Repository(options, args.data.resolve(), args.model)
        output({'ready': True})
        for line in sys.stdin.buffer:
            request_id = None
            try:
                request = json.loads(line)
                request_id = request['id']
                result = repository.dispatch(request['method'], request.get('params', {}))
                output({'id': request_id, 'result': result})
            except (psycopg.OperationalError, psycopg.InterfaceError):
                raise
            except Exception as error:
                output({'id': request_id, 'error': str(error)[:4000]})
    except Exception as error:
        output({'fatal': str(error)[:4000]})
    finally:
        if repository:
            repository.close()
        runtime.close()


if __name__ == '__main__':
    main()
