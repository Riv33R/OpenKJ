#!/usr/bin/env python3
"""Convert OPERATOR_GUIDE.md into a self-contained, offline-friendly OPERATOR_GUIDE.html."""

import html
import os
import re
from pathlib import Path


ALERT_META = {
    "NOTE": ("Примечание", "alert-note"),
    "TIP": ("Совет", "alert-tip"),
    "IMPORTANT": ("Важно", "alert-important"),
    "WARNING": ("Внимание", "alert-warning"),
    "CAUTION": ("Осторожно", "alert-caution"),
}


def slugify(text: str, used: dict) -> str:
    base = re.sub(r"[^\w\s-]", "", text.lower(), flags=re.UNICODE).strip()
    base = re.sub(r"[-\s]+", "-", base) or "section"
    count = used.get(base, 0)
    used[base] = count + 1
    return base if count == 0 else f"{base}-{count}"


def format_inline(text: str) -> str:
    code_spans = []

    def stash_code(match):
        code_spans.append(f"<code>{html.escape(match.group(1))}</code>")
        return f"\x00CODE{len(code_spans) - 1}\x00"

    text = re.sub(r"`([^`]+)`", stash_code, text)
    text = html.escape(text)
    text = re.sub(
        r"\[([^\]]+)\]\(([^)]+)\)",
        lambda m: f'<a href="{m.group(2)}" target="_blank" rel="noopener">{m.group(1)}</a>',
        text,
    )
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"<em>\1</em>", text)
    for idx, span in enumerate(code_spans):
        text = text.replace(f"\x00CODE{idx}\x00", span)
    return text


def parse_table(lines, start):
    rows = []
    i = start
    while i < len(lines) and lines[i].lstrip().startswith("|"):
        raw = lines[i].strip().strip("|")
        cells = [c.strip() for c in raw.split("|")]
        rows.append(cells)
        i += 1
    if len(rows) < 2 or not all(re.fullmatch(r":?-{3,}:?", c) for c in rows[1]):
        return None, start
    out = ['<div class="table-wrap"><table><thead><tr>']
    for cell in rows[0]:
        out.append(f"<th>{format_inline(cell)}</th>")
    out.append("</tr></thead><tbody>")
    for row in rows[2:]:
        out.append("<tr>")
        for cell in row:
            out.append(f"<td>{format_inline(cell)}</td>")
        out.append("</tr>")
    out.append("</tbody></table></div>")
    return "\n".join(out), i


def convert_markdown(md_text: str):
    lines = md_text.splitlines()
    html_parts = []
    toc = []
    used_slugs = {}
    i = 0
    n = len(lines)

    while i < n:
        line = lines[i]
        stripped = line.strip()

        if not stripped:
            i += 1
            continue

        if stripped.startswith("```"):
            lang = stripped[3:].strip()
            code_lines = []
            i += 1
            while i < n and not lines[i].strip().startswith("```"):
                code_lines.append(html.escape(lines[i]))
                i += 1
            i += 1
            cls = f' class="language-{html.escape(lang)}"' if lang else ""
            html_parts.append(f"<pre><code{cls}>" + "\n".join(code_lines) + "</code></pre>")
            continue

        if re.fullmatch(r"---+", stripped):
            html_parts.append("<hr>")
            i += 1
            continue

        m_head = re.match(r"^(#{1,4})\s+(.+)$", stripped)
        if m_head:
            level = len(m_head.group(1))
            raw_title = m_head.group(2).strip()
            slug = slugify(re.sub(r"[`*_\[\]()]", "", raw_title), used_slugs)
            if level in (2, 3):
                toc.append((level, slug, raw_title))
            html_parts.append(f'<h{level} id="{slug}">{format_inline(raw_title)}</h{level}>')
            i += 1
            continue

        if stripped.startswith(">"):
            quote_lines = []
            while i < n and lines[i].strip().startswith(">"):
                quote_lines.append(re.sub(r"^>\s?", "", lines[i].strip()))
                i += 1
            first = quote_lines[0] if quote_lines else ""
            m_alert = re.match(r"^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*(.*)$", first)
            if m_alert:
                kind = m_alert.group(1)
                label, css_cls = ALERT_META[kind]
                body_lines = ([m_alert.group(2)] if m_alert.group(2) else []) + quote_lines[1:]
                body_html = "<br>".join(format_inline(l) for l in body_lines if l)
                html_parts.append(
                    f'<div class="alert {css_cls}"><div class="alert-title">{label}</div><div class="alert-body">{body_html}</div></div>'
                )
            else:
                body_html = "<br>".join(format_inline(l) for l in quote_lines if l)
                html_parts.append(f"<blockquote>{body_html}</blockquote>")
            continue

        if stripped.startswith("|") and i + 1 < n and lines[i + 1].lstrip().startswith("|"):
            table_html, next_i = parse_table(lines, i)
            if table_html is not None:
                html_parts.append(table_html)
                i = next_i
                continue

        m_ul = re.match(r"^(\s*)[-*]\s+(.+)$", line)
        m_ol = re.match(r"^(\s*)\d+\.\s+(.+)$", line)
        if m_ul or m_ol:
            is_ol = m_ol is not None
            tag = "ol" if is_ol else "ul"
            items = []
            pattern = r"^\s*\d+\.\s+(.+)$" if is_ol else r"^\s*[-*]\s+(.+)$"
            while i < n:
                cur = lines[i]
                m_item = re.match(pattern, cur)
                if m_item:
                    items.append(format_inline(m_item.group(1)))
                    i += 1
                elif cur.startswith("  ") and items and cur.strip():
                    sub_m = re.match(r"^\s+[-*]\s+(.+)$", cur)
                    if sub_m:
                        items[-1] += f"<br>&bull; {format_inline(sub_m.group(1))}"
                    else:
                        items[-1] += " " + format_inline(cur.strip())
                    i += 1
                else:
                    break
            li_html = "\n".join(f"<li>{item}</li>" for item in items)
            html_parts.append(f"<{tag}>\n{li_html}\n</{tag}>")
            continue

        para_lines = []
        while i < n:
            cur = lines[i]
            s = cur.strip()
            if (
                not s
                or s.startswith("#")
                or s.startswith(">")
                or s.startswith("```")
                or re.fullmatch(r"---+", s)
                or re.match(r"^\s*[-*]\s+", cur)
                or re.match(r"^\s*\d+\.\s+", cur)
                or (s.startswith("|") and i + 1 < n and lines[i + 1].lstrip().startswith("|"))
            ):
                break
            para_lines.append(format_inline(s))
            i += 1
        if para_lines:
            html_parts.append("<p>" + " ".join(para_lines) + "</p>")

    return "\n".join(html_parts), toc


def build_html(md_path: Path, html_path: Path):
    md_text = md_path.read_text(encoding="utf-8")
    body_html, toc = convert_markdown(md_text)

    toc_items = []
    for level, slug, title in toc:
        cls = "toc-sub" if level == 3 else "toc-main"
        clean_title = html.escape(re.sub(r"[`*]", "", title))
        toc_items.append(f'<a class="{cls}" href="#{slug}">{clean_title}</a>')
    toc_html = "\n".join(toc_items)

    full_html = f"""<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>OpenKJ — Руководство оператора</title>
  <style>
    :root {{
      --bg: #f8fafc;
      --surface: #ffffff;
      --text: #1e293b;
      --muted: #64748b;
      --border: #e2e8f0;
      --accent: #2563eb;
      --accent-soft: #eff6ff;
      --code-bg: #f1f5f9;
      --shadow: 0 1px 3px rgba(15, 23, 42, 0.08);
    }}
    @media (prefers-color-scheme: dark) {{
      :root {{
        --bg: #0f172a;
        --surface: #1e293b;
        --text: #e2e8f0;
        --muted: #94a3b8;
        --border: #334155;
        --accent: #60a5fa;
        --accent-soft: #172554;
        --code-bg: #0f172a;
        --shadow: 0 1px 3px rgba(0, 0, 0, 0.35);
      }}
    }}
    * {{ box-sizing: border-box; }}
    html {{ scroll-behavior: smooth; }}
    body {{
      margin: 0;
      font-family: "Segoe UI", -apple-system, BlinkMacSystemFont, Roboto, Arial, sans-serif;
      background: var(--bg);
      color: var(--text);
      line-height: 1.65;
      font-size: 16px;
    }}
    .layout {{
      display: grid;
      grid-template-columns: 290px minmax(0, 1fr);
      max-width: 1320px;
      margin: 0 auto;
      gap: 28px;
      padding: 28px 24px 64px;
    }}
    .sidebar {{
      position: sticky;
      top: 20px;
      align-self: start;
      max-height: calc(100vh - 40px);
      overflow-y: auto;
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 18px 16px;
      box-shadow: var(--shadow);
    }}
    .sidebar-title {{
      font-size: 13px;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.06em;
      color: var(--muted);
      margin: 0 0 12px;
    }}
    .sidebar a {{
      display: block;
      text-decoration: none;
      color: var(--text);
      padding: 5px 8px;
      border-radius: 6px;
      font-size: 14px;
      line-height: 1.35;
    }}
    .sidebar a:hover {{
      background: var(--accent-soft);
      color: var(--accent);
    }}
    .sidebar a.toc-main {{ font-weight: 600; margin-top: 4px; }}
    .sidebar a.toc-sub {{ padding-left: 20px; font-size: 13px; color: var(--muted); }}
    .content {{
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 14px;
      padding: 36px 44px;
      box-shadow: var(--shadow);
      min-width: 0;
    }}
    h1 {{ font-size: 2rem; margin-top: 0; border-bottom: 2px solid var(--border); padding-bottom: 12px; }}
    h2 {{ font-size: 1.45rem; margin-top: 2.2rem; padding-top: 0.6rem; border-top: 1px solid var(--border); }}
    h3 {{ font-size: 1.15rem; margin-top: 1.5rem; color: var(--accent); }}
    a {{ color: var(--accent); }}
    code {{
      font-family: Consolas, "Cascadia Mono", monospace;
      font-size: 0.9em;
      background: var(--code-bg);
      padding: 2px 6px;
      border-radius: 5px;
      border: 1px solid var(--border);
    }}
    pre {{
      background: var(--code-bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 14px 16px;
      overflow-x: auto;
    }}
    pre code {{ border: none; padding: 0; background: transparent; }}
    hr {{ border: none; border-top: 1px solid var(--border); margin: 28px 0; }}
    blockquote {{
      margin: 16px 0;
      padding: 12px 16px;
      border-left: 4px solid var(--accent);
      background: var(--accent-soft);
      border-radius: 0 8px 8px 0;
    }}
    .alert {{
      margin: 16px 0;
      padding: 14px 16px;
      border-radius: 8px;
      border-left: 4px solid var(--accent);
      background: var(--accent-soft);
    }}
    .alert-title {{ font-weight: 700; margin-bottom: 4px; }}
    .alert-tip {{ border-left-color: #16a34a; }}
    .alert-tip .alert-title {{ color: #16a34a; }}
    .alert-warning, .alert-caution {{ border-left-color: #d97706; }}
    .alert-warning .alert-title, .alert-caution .alert-title {{ color: #d97706; }}
    .alert-important {{ border-left-color: #9333ea; }}
    .alert-important .alert-title {{ color: #9333ea; }}
    .table-wrap {{ overflow-x: auto; margin: 16px 0; }}
    table {{ width: 100%; border-collapse: collapse; font-size: 14.5px; }}
    th, td {{ border: 1px solid var(--border); padding: 9px 12px; text-align: left; vertical-align: top; }}
    th {{ background: var(--code-bg); font-weight: 600; }}
    ul, ol {{ padding-left: 24px; }}
    li {{ margin: 6px 0; }}
    @media (max-width: 960px) {{
      .layout {{ grid-template-columns: 1fr; padding: 14px; }}
      .sidebar {{ position: static; max-height: none; }}
      .content {{ padding: 22px 18px; }}
    }}
    @media print {{
      .sidebar {{ display: none; }}
      .layout {{ grid-template-columns: 1fr; padding: 0; }}
      .content {{ border: none; box-shadow: none; padding: 0; }}
    }}
  </style>
</head>
<body>
  <div class="layout">
    <nav class="sidebar" aria-label="Оглавление">
      <div class="sidebar-title">Содержание</div>
      {toc_html}
    </nav>
    <main class="content">
      {body_html}
    </main>
  </div>
</body>
</html>
"""
    html_path.write_text(full_html, encoding="utf-8")


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    build_html(root / "OPERATOR_GUIDE.md", root / "OPERATOR_GUIDE.html")
    print(f"Generated {root / 'OPERATOR_GUIDE.html'}")
