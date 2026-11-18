#!/usr/bin/env python3
"""Lightweight HTTPS-ready remote media library server for OpenKJ (behind Nginx Proxy Manager)."""

from __future__ import annotations

import gzip
import hashlib
import hmac
import json
import mimetypes
import os
import re
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Dict, List, Optional, Tuple
from urllib.parse import unquote, urlparse

try:
    import mutagen  # type: ignore
except ImportError:  # pragma: no cover - optional dependency
    mutagen = None

KARAOKE_EXTS = {".mp4", ".mkv", ".avi", ".webm"}
BREAK_EXTS = {".mp3", ".flac", ".ogg", ".wav", ".m4a"}


def parse_track_metadata(file_path: Path, track_type: str) -> Tuple[str, str, str]:
    """Extract (artist, title, song_id) from tags (if mutagen present) or filename."""
    stem = file_path.stem.strip()
    artist = ""
    title = stem
    song_id = ""

    if track_type == "break" and mutagen is not None:
        try:
            audio = mutagen.File(str(file_path), easy=True)
            if audio:
                artist = str((audio.get("artist") or [""])[0]).strip()
                tag_title = str((audio.get("title") or [""])[0]).strip()
                if tag_title:
                    title = tag_title
        except Exception:
            pass

    if not artist:
        parts = [p.strip() for p in re.split(r"\s+-\s+", stem) if p.strip()]
        if len(parts) >= 3:
            song_id = parts[0]
            artist = parts[1]
            title = " - ".join(parts[2:])
        elif len(parts) == 2:
            artist = parts[0]
            title = parts[1]

    return artist, title, song_id


class LibraryIndex:
    """Thread-safe in-memory index of karaoke and break music files."""

    def __init__(self, karaoke_dir: Optional[str], break_dir: Optional[str]) -> None:
        self.karaoke_dir = Path(karaoke_dir).resolve() if karaoke_dir else None
        self.break_dir = Path(break_dir).resolve() if break_dir else None
        self._lock = threading.RLock()
        self._tracks: List[dict] = []
        self._by_id: Dict[str, Tuple[dict, Path]] = {}
        self._last_scan = 0.0

    def scan(self) -> int:
        tracks: List[dict] = []
        by_id: Dict[str, Tuple[dict, Path]] = {}

        def scan_root(root: Optional[Path], track_type: str, exts: set) -> None:
            if not root or not root.exists() or not root.is_dir():
                return
            for dirpath, _, filenames in os.walk(root):
                for fname in sorted(filenames):
                    fpath = Path(dirpath) / fname
                    if fpath.suffix.lower() not in exts:
                        continue
                    try:
                        resolved = fpath.resolve()
                        if not resolved.is_relative_to(root):
                            continue
                        stat = resolved.stat()
                    except OSError:
                        continue

                    rel_path = resolved.relative_to(root).as_posix()
                    track_id = hashlib.sha1(f"{track_type}:{rel_path}".encode("utf-8")).hexdigest()[:16]
                    artist, title, song_id = parse_track_metadata(resolved, track_type)
                    entry = {
                        "id": track_id,
                        "type": track_type,
                        "artist": artist,
                        "title": title,
                        "songid": song_id,
                        "filename": resolved.name,
                        "rel_path": rel_path,
                        "ext": resolved.suffix.lower().lstrip("."),
                        "size": stat.st_size,
                        "mtime": int(stat.st_mtime),
                    }
                    tracks.append(entry)
                    by_id[track_id] = (entry, resolved)

        scan_root(self.karaoke_dir, "karaoke", KARAOKE_EXTS)
        scan_root(self.break_dir, "break", BREAK_EXTS)

        tracks.sort(key=lambda t: (t["type"], t["artist"].lower(), t["title"].lower(), t["filename"].lower()))

        with self._lock:
            self._tracks = tracks
            self._by_id = by_id
            self._last_scan = time.time()
            return len(self._tracks)

    def snapshot(self) -> Tuple[List[dict], float]:
        with self._lock:
            return list(self._tracks), self._last_scan

    def get_track(self, track_id: str) -> Optional[Tuple[dict, Path]]:
        with self._lock:
            return self._by_id.get(track_id)


class RemoteLibraryHandler(BaseHTTPRequestHandler):
    server_version = "OpenKJRemoteLibrary/1.0"

    def log_message(self, format: str, *args) -> None:  # pragma: no cover
        return

    @property
    def index(self) -> LibraryIndex:
        return self.server.library_index  # type: ignore[attr-defined]

    @property
    def api_token(self) -> str:
        return self.server.api_token  # type: ignore[attr-defined]

    def _is_authorized(self) -> bool:
        expected = self.api_token
        if not expected:
            return False
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            return False
        provided = auth[len("Bearer "):].strip()
        return hmac.compare_digest(provided.encode("utf-8"), expected.encode("utf-8"))

    def _send_json(self, status: int, payload: dict, allow_gzip: bool = False) -> None:
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        use_gzip = allow_gzip and "gzip" in self.headers.get("Accept-Encoding", "").lower()
        body = gzip.compress(raw) if use_gzip else raw
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        if use_gzip:
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _require_auth(self) -> bool:
        if not self._is_authorized():
            self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "Unauthorized"})
            return False
        return True

    def do_GET(self) -> None:
        if not self._require_auth():
            return

        parsed = urlparse(self.path)
        path = unquote(parsed.path)

        if path == "/api/v1/health":
            tracks, last_scan = self.index.snapshot()
            self._send_json(
                HTTPStatus.OK,
                {
                    "status": "ok",
                    "tracks": len(tracks),
                    "last_scan": int(last_scan),
                },
            )
            return

        if path == "/api/v1/catalog":
            tracks, last_scan = self.index.snapshot()
            self._send_json(
                HTTPStatus.OK,
                {
                    "version": 1,
                    "generated_at": int(last_scan),
                    "tracks": tracks,
                },
                allow_gzip=True,
            )
            return

        if path.startswith("/api/v1/download/"):
            track_id = path[len("/api/v1/download/"):].strip("/")
            item = self.index.get_track(track_id)
            if item is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "Track not found"})
                return

            entry, file_path = item
            if not file_path.exists():
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "File missing on disk"})
                return

            file_size = file_path.stat().st_size
            ctype, _ = mimetypes.guess_type(str(file_path))
            ctype = ctype or "application/octet-stream"

            range_hdr = self.headers.get("Range")
            start = 0
            end = file_size - 1
            status = HTTPStatus.OK

            if range_hdr and range_hdr.startswith("bytes="):
                spec = range_hdr[len("bytes="):].split(",")[0].strip()
                if "-" in spec:
                    s_part, e_part = spec.split("-", 1)
                    try:
                        if s_part:
                            start = int(s_part)
                            end = int(e_part) if e_part else file_size - 1
                        elif e_part:
                            suffix_len = int(e_part)
                            start = max(0, file_size - suffix_len)
                            end = file_size - 1
                        if start < 0 or start > end or end >= file_size:
                            self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                            self.send_header("Content-Range", f"bytes */{file_size}")
                            self.end_headers()
                            return
                        status = HTTPStatus.PARTIAL_CONTENT
                    except ValueError:
                        pass

            length = end - start + 1
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            if status == HTTPStatus.PARTIAL_CONTENT:
                self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
            self.send_header("Content-Disposition", f'attachment; filename="{entry["filename"]}"')
            self.end_headers()

            with open(file_path, "rb") as f:
                f.seek(start)
                remaining = length
                chunk_size = 256 * 1024
                while remaining > 0:
                    chunk = f.read(min(chunk_size, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"error": "Not found"})

    def do_POST(self) -> None:
        if not self._require_auth():
            return
        parsed = urlparse(self.path)
        if parsed.path == "/api/v1/rescan":
            count = self.index.scan()
            self._send_json(HTTPStatus.OK, {"status": "ok", "tracks": count})
            return
        self._send_json(HTTPStatus.NOT_FOUND, {"error": "Not found"})


class RemoteLibraryServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, server_address, RequestHandlerClass, library_index: LibraryIndex, api_token: str):
        self.library_index = library_index
        self.api_token = api_token
        super().__init__(server_address, RequestHandlerClass)


def main() -> None:
    token = os.environ.get("OPENKJ_API_TOKEN", "").strip()
    if not token:
        raise SystemExit("OPENKJ_API_TOKEN environment variable is required")

    karaoke_dir = os.environ.get("OPENKJ_KARAOKE_DIR", "/media/karaoke")
    break_dir = os.environ.get("OPENKJ_BREAK_DIR", "/media/break")
    host = os.environ.get("OPENKJ_HOST", "0.0.0.0")
    port = int(os.environ.get("OPENKJ_PORT", "8080"))

    index = LibraryIndex(karaoke_dir=karaoke_dir, break_dir=break_dir)
    count = index.scan()
    print(f"OpenKJ Remote Library indexed {count} tracks. Listening on {host}:{port}")

    srv = RemoteLibraryServer((host, port), RemoteLibraryHandler, index, token)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
