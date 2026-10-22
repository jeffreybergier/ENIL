#!/usr/bin/env python3
"""Offline synthetic chat benchmark. Run from anywhere; outputs stay in build/."""
import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import sqlite3
import statistics
import subprocess

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
WORKLOAD = 'mixed-chat-v2'


def run(args, **kwargs):
    return subprocess.check_output(args, text=True, timeout=300, **kwargs)


def css_from_source():
    """Read the actual Objective-C literal defaults, rather than stale CSS copies."""
    source = (REPO / 'source/shared/ENILUserDefaults.m').read_text()
    css = ''
    for name in ['commonCSS', 'messageCSS']:
        body = source.split('+ (NSString *)' + name + ';', 1)[1].split('\n}', 1)[0]
        literals = re.findall(r'@?("(?:[^"\\]|\\.)*")', body)
        if not literals:
            raise RuntimeError('CSS layout changed; update benchmark extraction')
        css += ''.join(json.loads(s) for s in literals)
    return css


def assets(root):
    from PIL import Image, ImageDraw
    folder = root / 'assets'
    folder.mkdir(exist_ok=True)
    for i in range(64):
        img = Image.new('RGB', (640, 480), (i * 31 % 256, i * 47 % 256, i * 59 % 256))
        draw = ImageDraw.Draw(img)
        for y in range(0, 480, 12):
            draw.line((0, y, 640, (y + i * 17) % 480), fill=(240, 180, i * 13 % 256), width=4)
        img.save(folder / f'photo-{i}.png')
    for i in range(16):
        img = Image.new('RGB', (180, 180), (i * 17 % 256, 180, 90))
        draw = ImageDraw.Draw(img)
        draw.ellipse((10, 10, 170, 170), fill=(255, 220, 30))
        draw.ellipse((45, 50, 60, 65), fill='black')
        draw.ellipse((120, 50, 135, 65), fill='black')
        draw.arc((45, 60, 135, 140), 0, 180, fill='black', width=6)
        img.save(folder / f'emoji-{i}.png')
    # Include real image decoding and bundled reaction fonts without fetching assets.
    fonts = root / 'fonts'
    fonts.mkdir(exist_ok=True)
    source = Path('/altivec/libs/cocoa/build-phone/Resources/Fonts')
    for name in ['FA7-Solid-900.otf', 'FA7-Regular-400.otf', 'FA7-Brands-400.otf']:
        shutil.copyfile(source / name, fonts / name)


def fixture(path, count):
    with closing(sqlite3.connect(path)) as db, db:
        db.create_collation('LOCALIZED', lambda a, b: (a > b) - (a < b))
        for i in range(16):
            for table, key in [('sticons_v2', 'sticon_id'), ('stickers_v2', 'sticker_id')]:
                db.execute(f'INSERT INTO {table} ({key},package_id,image_path,image_width,image_height) VALUES(?,?,?,?,?)',
                           (str(i), 'bench', f'assets/emoji-{i}.png', 180, 180))
        db.execute("INSERT INTO contacts_v2(mid,displayName) VALUES('peer','Synthetic sender')")
        for i in range(count):
            kind = i % 5
            text = (f'Message {i:04d}: 日本語 😀 ordinary parentheses (keep this) & <text>\n' +
                    'Long wrapped text, dollar amounts $25, punctuation :( and links https://example.invalid/. ' * 6)
            spans = []
            if kind in (1, 3):
                if kind == 3:
                    text = ''
                for j in range(3):
                    start = len(text.encode('utf-16-le')) // 2
                    text += '(smile)'
                    spans.append({'S': start, 'E': start + 7, 'productId': 'bench', 'sticonId': str((i+j) % 16)})
                    text += ' '
            image = f'assets/photo-{i % 64}.png' if kind == 2 else None
            db.execute('''INSERT INTO messages_v2
                (id,chat_id,"from","to",createdTime,contentType,text,replace_json,
                 media_path,thumb_path,orig_width,orig_height,thumb_width,thumb_height,
                 sticker_id,sticker_pkg_id,reactions_json,decrypt_status)
                VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1)''',
                (f'{i:06d}', 'u-benchmark', 'self' if i % 2 else 'peer', 'u-benchmark',
                 1700000000000 + i * 60000, 1 if kind == 2 else 7 if kind == 4 else 0,
                 text if kind not in (2, 4) else '',
                 json.dumps({'sticon': {'resources': spans}}) if spans else None,
                 image, image, 640, 480, 640, 480,
                 str(i % 16) if kind == 4 else None, 'bench' if kind == 4 else None,
                 '[{"predefinedReactionType":2,"count":3}]' if i % 7 == 0 else None))
            for j, span in enumerate(spans):
                db.execute('INSERT INTO message_sticons VALUES(?,?,?,?)',
                           (f'{i:06d}', j, 'bench', span['sticonId']))


def summary(samples):
    ordered = sorted(samples)
    return {'median': statistics.median(samples), 'min': ordered[0],
            'max': ordered[-1], 'samples': samples}


def compare_results(result, baseline, enforce=False, threshold=25):
    if (baseline['workload'], baseline['messages']) != (result['workload'], result['messages']):
        raise SystemExit('Incompatible baseline workload/message count')
    if baseline['environment'] != result['environment']:
        print('Environment differs from baseline; timing comparison is advisory.')
        if enforce:
            raise SystemExit('Cannot enforce timings across different environments')
    regressed = []
    for key, value in result['metrics'].items():
        reference = baseline['metrics'][key]['median']
        if reference <= 0:
            raise SystemExit('Invalid baseline median: ' + key)
        change = (value['median'] / reference - 1) * 100
        print(f'{key}: {change:+.1f}% vs baseline')
        if change > threshold:
            regressed.append(key)
    if enforce and regressed:
        raise SystemExit('Regression threshold exceeded: ' + ', '.join(regressed))


def main():
    global REPO
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=REPO,
                   help='benchmark production code/CSS from another checkout using this harness')
    p.add_argument('--messages', type=int, default=2000)
    p.add_argument('--repeats', type=int, default=7)
    p.add_argument('--browser', default=os.environ.get('CHROMIUM'))
    p.add_argument('--compare', type=Path)
    p.add_argument('--max-regression-percent', type=float, default=25)
    p.add_argument('--enforce', action='store_true', help='fail on timing regressions (only on a controlled runner)')
    args = p.parse_args()
    REPO = args.source_root.resolve()
    if args.messages < 5 or args.messages % 5 or args.repeats < 3:
        p.error('messages must be a positive multiple of 5; repeats must be >= 3')
    browser = args.browser or shutil.which('chromium') or shutil.which('google-chrome')
    if not browser:
        candidates = sorted(Path('/opt/playwright-browsers').glob('chromium-*/chrome-linux/chrome'))
        browser = str(candidates[-1]) if candidates else None
    if not browser:
        p.error('Chromium is required; pass --browser PATH or set CHROMIUM')
    out = REPO / 'build/render-benchmark'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'chats').mkdir(exist_ok=True)
    binary = out / 'render-benchmark'
    flags = shlex.split(run(['pkg-config', '--cflags', '--libs', 'libcjson', 'sqlite3', 'libcurl', 'openssl']))
    compile_cmd = ['cc', '-O2', '-g', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                   '-Wno-deprecated-declarations', '-Wno-format-truncation',
                   '-ffunction-sections', '-fdata-sections', '-Isource/shared',
                   '-Isource/deps/qrcodegen/c', str(HERE / 'render_benchmark.c'),
                   *map(str, sorted((REPO / 'source/shared').glob('*.c'))),
                   '-Wl,--gc-sections', '-Wl,--wrap=curl_easy_perform', '-pthread', *flags, '-lz', '-o', str(binary)]
    print('Compiling production C renderer (-O2)...', flush=True)
    subprocess.run(compile_cmd, cwd=REPO, check=True, timeout=180)
    db = out / 'fixture.sqlite'
    for suffix in ['', '-wal', '-shm']:
        Path(str(db) + suffix).unlink(missing_ok=True)
    run([str(binary), 'init', str(db)])
    fixture(db, args.messages)
    assets(out)
    css = css_from_source()
    (out / 'message.css').write_text(css)
    print(f'Rendering {args.messages} mixed messages; two warmups + {args.repeats} samples...', flush=True)
    native = run([str(binary), 'render', str(db), str(out / 'message.css'),
                  str(out / 'chats/chat.html'), str(args.messages), str(args.repeats), str(out / 'fonts')],
                 env={**os.environ, 'TZ': 'UTC', 'LC_ALL': 'C'})
    html = (out / 'chats/chat.html').read_text()
    if len(re.findall(r'id="msg-\d+"', html)) != args.messages:
        raise RuntimeError('Renderer did not produce the complete fixture')
    # Legacy pages omit viewport metadata, making mobile Chromium use a
    # 980-CSS-pixel layout on the emulated 320-pixel screen. Compare equal
    # layout widths without modifying the production renderer's saved output.
    viewport_injected = not re.search(r'<meta\b[^>]*name=[\"\']viewport[\"\']', html, re.I)
    browser_html = html
    if viewport_injected:
        browser_html = html.replace('</head>',
            '<meta name="viewport" content="width=device-width,initial-scale=1"></head>', 1)
    browser_page = out / 'chats/browser.html'
    browser_page.write_text(browser_html)
    # Keep interactive controls outside the production viewport and document.
    demo_script = (HERE / 'render_scroll_demo.js').read_text()
    (out / 'chats/scroll-demo.html').write_text(browser_html.replace(
        '</body>', '<script>' + demo_script + '</script></body>'))
    shutil.copyfile(HERE / 'render_scroll_demo.html', out / 'scroll-demo.html')
    with (out / 'browser.log').open('w') as log:
        browser_result = json.loads(run(['node', str(HERE / 'render_benchmark_browser.mjs'), browser,
                                        str(browser_page), str(args.messages), str(args.repeats)], stderr=log))
    metrics = {'native_render_ms': summary([float(s) for s in native.splitlines()])}
    for key in ['load_ms', 'scroll_ms', 'append_layout_ms', 'prepend_layout_ms']:
        metrics['browser_' + key] = summary([s[key] for s in browser_result['samples']])
    result = {'workload': WORKLOAD, 'messages': args.messages, 'warmups': 2, 'repeats': args.repeats,
              'revision': run(['git', 'rev-parse', 'HEAD'], cwd=REPO).strip(),
              'dirty': bool(run(['git', 'status', '--porcelain'], cwd=REPO).strip()),
              'environment': {'platform': platform.platform(), 'machine': platform.machine(),
                              'cpu': next((line.split(':', 1)[1].strip() for line in
                                           Path('/proc/cpuinfo').read_text().splitlines()
                                           if line.startswith(('model name', 'Hardware'))), platform.machine()),
                              'logical_cpus': os.cpu_count(),
                              'allowed_cpus': len(os.sched_getaffinity(0)),
                              'compiler': run(['cc', '--version']).splitlines()[0],
                              'browser': browser_result['version'], 'viewport': browser_result['viewport'],
                              'node': run(['node', '--version']).strip()},
              'browser_viewport_injected': viewport_injected,
              'prepend_anchor_max_drift_px': max(s['prepend_anchor_drift_px'] for s in browser_result['samples']),
              'html_bytes': len(html.encode()), 'css_sha256': hashlib.sha256(css.encode()).hexdigest(),
              'dom': {k: browser_result['samples'][0][k] for k in ['dom_nodes', 'images', 'emojis']},
              'metrics': metrics}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    for key, value in metrics.items():
        print(f'{key}: median {value["median"]:.2f} ms ({value["min"]:.2f}–{value["max"]:.2f})')
    print('Maximum prepend scroll-anchor drift:', result['prepend_anchor_max_drift_px'], 'px')
    print('Results and page:', out)
    if args.compare:
        compare_results(result, json.loads(args.compare.read_text()),
                        args.enforce, args.max_regression_percent)



if __name__ == '__main__':
    main()
