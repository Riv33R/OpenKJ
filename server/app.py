import hashlib
import hmac
import os
import re
import sqlite3
import time
from contextlib import closing
from pathlib import Path
from flask import Flask, jsonify, render_template, request, send_file

app = Flask(__name__)
app.secret_key = os.environ.get('SECRET_KEY', os.urandom(24).hex())

DB_PATH = os.environ.get('DB_PATH', '/app/data/karaoke.db')
MEDIA_DIR = os.environ.get('OPENKJ_KARAOKE_DIR', '/media/karaoke')
BREAK_DIR = os.environ.get('OPENKJ_BREAK_DIR', '/media/break')
ADMIN_PASSWORD = (os.environ.get('ADMIN_PASSWORD') or os.environ.get('DJ_PASSWORD') or '').strip()
API_TOKEN = (os.environ.get('OPENKJ_API_TOKEN') or os.environ.get('DJ_PASSWORD') or ADMIN_PASSWORD).strip()

KARAOKE_EXTS = {'.mp4', '.mkv', '.avi', '.webm', '.cdg', '.zip', '.mp3'}
BREAK_EXTS = {'.mp3', '.flac', '.ogg', '.wav', '.m4a'}


def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    db_dir = os.path.dirname(DB_PATH)
    if db_dir:
        os.makedirs(db_dir, exist_ok=True)
    with closing(get_db()) as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS songs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                artist TEXT NOT NULL,
                title TEXT NOT NULL,
                filename TEXT NOT NULL,
                filepath TEXT UNIQUE NOT NULL,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """)
        conn.commit()


def parse_filename_meta(fname: str):
    stem = os.path.splitext(fname)[0].strip()
    artist = "Разное"
    title = stem
    song_id = ""
    parts = [p.strip() for p in re.split(r'\s+[-\u2014]\s+', stem) if p.strip()]
    if len(parts) >= 3:
        song_id = parts[0]
        artist = parts[1]
        title = " - ".join(parts[2:])
    elif len(parts) == 2:
        artist, title = parts[0], parts[1]
    return artist, title, song_id


def scan_media():
    init_db()
    if not os.path.exists(MEDIA_DIR):
        return 0, 0
    found_count = 0
    added_count = 0

    with closing(get_db()) as conn:
        for root, _, files in os.walk(MEDIA_DIR):
            for f in sorted(files):
                ext = os.path.splitext(f)[1].lower()
                if ext in KARAOKE_EXTS:
                    found_count += 1
                    rel_path = os.path.relpath(os.path.join(root, f), MEDIA_DIR)
                    artist, title, _ = parse_filename_meta(f)
                    cur = conn.execute(
                        'INSERT OR IGNORE INTO songs (artist, title, filename, filepath) VALUES (?, ?, ?, ?)',
                        (artist, title, f, rel_path)
                    )
                    if cur.rowcount > 0:
                        added_count += 1
        conn.commit()
    return found_count, added_count


def build_v1_catalog():
    tracks = []
    by_id = {}

    def scan_root(root_dir: str, track_type: str, exts: set):
        root = Path(root_dir)
        if not root.exists() or not root.is_dir():
            return
        root_resolved = root.resolve()
        for dirpath, _, filenames in os.walk(root_resolved):
            for fname in sorted(filenames):
                fpath = Path(dirpath) / fname
                if fpath.suffix.lower() not in exts:
                    continue
                try:
                    resolved = fpath.resolve()
                    if not resolved.is_relative_to(root_resolved):
                        continue
                    stat = resolved.stat()
                except OSError:
                    continue
                rel_path = resolved.relative_to(root_resolved).as_posix()
                track_id = hashlib.sha1(f"{track_type}:{rel_path}".encode("utf-8")).hexdigest()[:16]
                artist, title, song_id = parse_filename_meta(resolved.name)
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

    scan_root(MEDIA_DIR, "karaoke", KARAOKE_EXTS)
    scan_root(BREAK_DIR, "break", BREAK_EXTS)
    tracks.sort(key=lambda t: (t["type"], t["artist"].lower(), t["title"].lower(), t["filename"].lower()))
    return tracks, by_id


def check_bearer_auth() -> bool:
    token = (os.environ.get('OPENKJ_API_TOKEN') or os.environ.get('DJ_PASSWORD') or API_TOKEN).strip()
    if not token:
        return False
    auth = request.headers.get("Authorization", "")
    if not auth.startswith("Bearer "):
        return False
    provided = auth[len("Bearer "):].strip()
    return hmac.compare_digest(provided.encode("utf-8"), token.encode("utf-8"))


try:
    scan_media()
except Exception as e:
    print(f"Scan error: {e}")


@app.route('/')
def index():
    return render_template('index.html')


@app.route('/api/songs')
def api_songs():
    init_db()
    q = request.args.get('q', '').strip()
    letter = request.args.get('letter', '').strip().upper()
    limit = int(request.args.get('limit', 200))

    with closing(get_db()) as conn:
        query = 'SELECT id, artist, title, filename FROM songs'
        params = []
        conditions = []

        if q:
            pattern = f'%{q}%'
            conditions.append('(artist LIKE ? OR title LIKE ?)')
            params.extend([pattern, pattern])

        if letter:
            if letter == 'A-Z':
                conditions.append("UPPER(SUBSTR(artist, 1, 1)) BETWEEN 'A' AND 'Z'")
            elif letter == '0-9':
                conditions.append("SUBSTR(artist, 1, 1) BETWEEN '0' AND '9'")
            else:
                conditions.append("UPPER(SUBSTR(artist, 1, 1)) = ?")
                params.append(letter)

        if conditions:
            query += ' WHERE ' + ' AND '.join(conditions)

        query += ' ORDER BY artist COLLATE NOCASE, title COLLATE NOCASE LIMIT ?'
        params.append(limit)

        cur = conn.execute(query, params)
        songs = [dict(row) for row in cur.fetchall()]
    return jsonify(songs)


@app.route('/api/stats')
def api_stats():
    init_db()
    with closing(get_db()) as conn:
        total_songs = conn.execute('SELECT COUNT(*) as c FROM songs').fetchone()['c']
        total_artists = conn.execute('SELECT COUNT(DISTINCT artist) as c FROM songs').fetchone()['c']
    return jsonify({
        'total_songs': total_songs,
        'total_artists': total_artists
    })


@app.route('/api/scan', methods=['POST'])
def api_scan():
    data = request.json or request.form or {}
    pwd = data.get('password', '')
    admin_pwd = (os.environ.get('ADMIN_PASSWORD') or os.environ.get('DJ_PASSWORD') or ADMIN_PASSWORD).strip()
    if not admin_pwd or not hmac.compare_digest(pwd.encode('utf-8'), admin_pwd.encode('utf-8')):
        return jsonify({'error': 'Неверный пароль администратора'}), 403
    found, added = scan_media()
    return jsonify({'status': 'ok', 'found': found, 'added': added})


@app.route('/api/v1/health', methods=['GET'])
def api_v1_health():
    if not check_bearer_auth():
        return jsonify({'error': 'Unauthorized'}), 401
    tracks, _ = build_v1_catalog()
    return jsonify({'status': 'ok', 'tracks': len(tracks), 'last_scan': int(time.time())})


@app.route('/api/v1/catalog', methods=['GET'])
def api_v1_catalog():
    if not check_bearer_auth():
        return jsonify({'error': 'Unauthorized'}), 401
    tracks, _ = build_v1_catalog()
    return jsonify({'version': 1, 'generated_at': int(time.time()), 'tracks': tracks})


@app.route('/api/v1/rescan', methods=['POST'])
def api_v1_rescan():
    if not check_bearer_auth():
        return jsonify({'error': 'Unauthorized'}), 401
    scan_media()
    tracks, _ = build_v1_catalog()
    return jsonify({'status': 'ok', 'tracks': len(tracks)})


@app.route('/api/v1/download/<track_id>', methods=['GET'])
def api_v1_download(track_id):
    if not check_bearer_auth():
        return jsonify({'error': 'Unauthorized'}), 401
    _, by_id = build_v1_catalog()
    item = by_id.get(track_id)
    if item is None:
        return jsonify({'error': 'Track not found'}), 404
    entry, file_path = item
    if not file_path.exists():
        return jsonify({'error': 'File missing on disk'}), 404

    return send_file(file_path, as_attachment=True, download_name=entry['filename'], conditional=True)


@app.route('/api.php', methods=['GET', 'POST'])
@app.route('/api', methods=['GET', 'POST'])
def openkj_api():
    init_db()
    data = request.get_json(silent=True) or {}
    command = data.get('command') or request.args.get('command', '')

    if command in ('venueExists', 'connectionTest'):
        return jsonify({'command': command, 'error': False, 'exists': True})

    if command in ('venueAccepting', 'getAccepting'):
        return jsonify({'command': command, 'accepting': False})

    if command == 'search':
        search_str = data.get('searchString', '')
        with closing(get_db()) as conn:
            pattern = f'%{search_str}%'
            cur = conn.execute(
                'SELECT id as song_id, artist, title FROM songs WHERE artist LIKE ? OR title LIKE ? LIMIT 100',
                (pattern, pattern)
            )
            results = [dict(row) for row in cur.fetchall()]
        return jsonify({'command': command, 'results': results})

    if command == 'getRequests':
        return jsonify({'command': command, 'requests': []})

    return jsonify({'command': command, 'status': 'ok'})


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8080)
