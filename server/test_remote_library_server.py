import os
import tempfile
import unittest
from pathlib import Path

try:
    import flask  # noqa: F401
    HAS_FLASK = True
except ImportError:
    HAS_FLASK = False


@unittest.skipUnless(HAS_FLASK, "Flask not installed in local Python environment")
class TestOpenKJServerApp(unittest.TestCase):
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

        os.environ["DB_PATH"] = str(root / "karaoke.db")
        os.environ["OPENKJ_KARAOKE_DIR"] = str(self.kar_dir)
        os.environ["OPENKJ_BREAK_DIR"] = str(self.brk_dir)
        os.environ["OPENKJ_API_TOKEN"] = "super-secret-token"
        os.environ["DJ_PASSWORD"] = "super-secret-token"

        import importlib
        import app as server_app
        importlib.reload(server_app)
        self.server_app = server_app
        self.client = server_app.app.test_client()

    def tearDown(self):
        self.tmp.cleanup()

    def test_auth_required(self):
        resp = self.client.get("/api/v1/health", headers={"Authorization": "Bearer wrong"})
        self.assertEqual(resp.status_code, 401)

    def test_web_catalog_and_v1_download_range(self):
        web_resp = self.client.get("/")
        self.assertEqual(web_resp.status_code, 200)

        songs_resp = self.client.get("/api/songs?q=Kino")
        self.assertEqual(songs_resp.status_code, 200)
        self.assertEqual(len(songs_resp.get_json()), 1)

        cat_resp = self.client.get("/api/v1/catalog", headers={"Authorization": "Bearer super-secret-token"})
        self.assertEqual(cat_resp.status_code, 200)
        tracks = cat_resp.get_json()["tracks"]
        self.assertEqual(len(tracks), 2)
        kar = next(t for t in tracks if t["type"] == "karaoke")
        self.assertEqual(kar["artist"], "Kino")
        self.assertEqual(kar["title"], "Gruppa Krovi")
        self.assertEqual(kar["songid"], "RUS001")

        dl_resp = self.client.get(
            f"/api/v1/download/{kar['id']}",
            headers={"Authorization": "Bearer super-secret-token", "Range": "bytes=5-12"},
        )
        self.assertEqual(dl_resp.status_code, 206)
        self.assertEqual(dl_resp.data, b"MP4_DATA")
        dl_resp.close()

    def test_rescan(self):
        (self.kar_dir / "Artist - New Song.mkv").write_bytes(b"MKV")
        r = self.client.post("/api/v1/rescan", headers={"Authorization": "Bearer super-secret-token"})
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.get_json()["tracks"], 3)


if __name__ == "__main__":
    unittest.main()
