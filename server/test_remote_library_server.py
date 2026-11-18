import gzip
import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

from remote_library_server import LibraryIndex, RemoteLibraryHandler, RemoteLibraryServer


class TestRemoteLibraryServer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.kar_dir = root / "karaoke"
        self.brk_dir = root / "break"
        self.kar_dir.mkdir()
        self.brk_dir.mkdir()

        (self.kar_dir / "RUS001 - Kino - Gruppa Krovi.mp4").write_bytes(b"FAKE_MP4_DATA_0123456789")
        (self.brk_dir / "Daft Punk - Instant Crush.mp3").write_bytes(b"FAKE_MP3_DATA_ABCDEF")
        (self.kar_dir / "ignored.txt").write_text("ignore me", encoding="utf-8")

        self.token = "super-secret-token"
        self.index = LibraryIndex(str(self.kar_dir), str(self.brk_dir))
        self.index.scan()
        self.server = RemoteLibraryServer(("127.0.0.1", 0), RemoteLibraryHandler, self.index, self.token)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def _open(self, path, token=None, headers=None, method="GET"):
        hdrs = dict(headers or {})
        if token is not None:
            hdrs["Authorization"] = f"Bearer {token}"
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", headers=hdrs, method=method)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        return opener.open(req)

    def test_auth_required(self):
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            self._open("/api/v1/health", token="wrong")
        self.assertEqual(ctx.exception.code, 401)

    def test_catalog_and_download_range(self):
        with self._open("/api/v1/catalog", token=self.token, headers={"Accept-Encoding": "gzip"}) as resp:
            self.assertEqual(resp.status, 200)
            raw = resp.read()
            if resp.headers.get("Content-Encoding") == "gzip":
                raw = gzip.decompress(raw)
            data = json.loads(raw.decode("utf-8"))

        tracks = data["tracks"]
        self.assertEqual(len(tracks), 2)
        kar = next(t for t in tracks if t["type"] == "karaoke")
        self.assertEqual(kar["artist"], "Kino")
        self.assertEqual(kar["title"], "Gruppa Krovi")
        self.assertEqual(kar["songid"], "RUS001")

        # Range download
        with self._open(f"/api/v1/download/{kar['id']}", token=self.token, headers={"Range": "bytes=5-12"}) as dresp:
            self.assertEqual(dresp.status, 206)
            self.assertEqual(dresp.read(), b"MP4_DATA")

    def test_rescan(self):
        (self.kar_dir / "Artist - New Song.mkv").write_bytes(b"MKV")
        with self._open("/api/v1/rescan", token=self.token, method="POST") as resp:
            self.assertEqual(resp.status, 200)
            data = json.loads(resp.read().decode("utf-8"))
            self.assertEqual(data["tracks"], 3)


if __name__ == "__main__":
    unittest.main()
