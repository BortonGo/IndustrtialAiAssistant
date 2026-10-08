"""Private, loopback-only PostgreSQL cluster; no services or PATH changes."""
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile

import psycopg
from psycopg import sql


class LocalPostgres:
    def __init__(self, root: Path, data: Path, port=55432):
        self.bin = root / "tools/postgresql/bin"
        self.data = data.resolve()
        self.cluster = self.data / "postgres"
        self.port = port
        self.lock = None
        self.owned = False

    def command(self, program, *args, check=True):
        # Windows server descendants may inherit pipe handles. A file avoids waiting
        # for EOF on a pipe retained by the long-lived postgres process.
        with tempfile.TemporaryFile() as output:
            result = subprocess.run([str(self.bin / (program + ".exe")), *map(str, args)],
                                    stdin=subprocess.DEVNULL, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=90,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            output.seek(0)
            result.stdout = output.read()
        if check and result.returncode:
            raise RuntimeError(result.stdout.decode("utf-8", errors="replace")[-3000:])
        return result

    def start(self):
        if not (self.bin / "pg_ctl.exe").is_file():
            raise RuntimeError("Нет tools/postgresql/bin/pg_ctl.exe. Подготовьте PostgreSQL по README.")
        self.data.mkdir(parents=True, exist_ok=True)
        self.lock = (self.data / "application.lock").open("a+b")
        self.lock.seek(0)
        if os.name == "nt":
            import msvcrt
            try:
                msvcrt.locking(self.lock.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as error:
                raise RuntimeError("Эта папка БД уже используется другим экземпляром приложения") from error
        credentials_file = self.data / "credentials.json"
        if not credentials_file.exists():
            if (self.cluster / "PG_VERSION").exists():
                raise RuntimeError("Для существующей БД отсутствует credentials.json; база не изменена")
            credentials_file.write_text(json.dumps({"admin": secrets.token_urlsafe(32),
                                                    "app": secrets.token_urlsafe(32)}), encoding="utf-8")
        credentials = json.loads(credentials_file.read_text(encoding="utf-8"))
        if not (self.cluster / "PG_VERSION").exists():
            pwfile = self.data / "init-password.tmp"
            try:
                pwfile.write_text(credentials["admin"], encoding="utf-8")
                self.command("initdb", "-D", self.cluster, "-U", "local_assistant_admin",
                             "--encoding=UTF8", "--locale=C", "--auth=scram-sha-256", "--pwfile", pwfile)
            finally:
                pwfile.unlink(missing_ok=True)
            with (self.cluster / "postgresql.conf").open("a", encoding="utf-8") as config:
                config.write(f"\nlisten_addresses = '127.0.0.1'\nport = {self.port}\n"
                             "shared_buffers = '64MB'\nmax_connections = 10\n")
        if self.command("pg_ctl", "-D", self.cluster, "status", check=False).returncode:
            try:
                self.command("pg_ctl", "-D", self.cluster, "-l", self.data / "postgres.log", "-o", f"-p {self.port}", "-w", "start")
            except RuntimeError as error:
                log = (self.data / "postgres.log").read_text(encoding="utf-8", errors="replace")[-2000:]
                raise RuntimeError(f"PostgreSQL не запущен на порту {self.port}: {error}\n{log}") from error
        admin_options = dict(host="127.0.0.1", port=self.port, dbname="postgres",
                             user="local_assistant_admin", password=credentials["admin"], connect_timeout=5)
        with psycopg.connect(**admin_options, autocommit=True) as connection:
            actual = Path(connection.execute("SHOW data_directory").fetchone()[0]).resolve()
            if actual != self.cluster.resolve():
                raise RuntimeError("Порт БД занят другим кластером PostgreSQL")
            self.owned = True
            if not connection.execute("SELECT 1 FROM pg_roles WHERE rolname='local_assistant'").fetchone():
                connection.execute(sql.SQL("CREATE ROLE local_assistant LOGIN PASSWORD {}").format(sql.Literal(credentials["app"])))
            if not connection.execute("SELECT 1 FROM pg_database WHERE datname='local_assistant'").fetchone():
                connection.execute("CREATE DATABASE local_assistant OWNER local_assistant")
        return dict(host="127.0.0.1", port=self.port, dbname="local_assistant",
                    user="local_assistant", password=credentials["app"], connect_timeout=5)

    def close(self):
        if self.owned:
            self.command("pg_ctl", "-D", self.cluster, "-m", "fast", "-w", "stop", check=False)
            self.owned = False
        if self.lock:
            self.lock.close()
            self.lock = None
