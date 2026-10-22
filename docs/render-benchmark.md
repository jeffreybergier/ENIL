# Offline Linux rendering benchmark

Run `make benchmark-render` to measure a synthetic, deliberately heavy chat with
2,000 messages loaded at once. This is a stress workload, not the app's normal
40-message initial page. It does not use a real account, contact a LINE service,
or access a device. No production app code is replaced by benchmark code.

Requirements: Linux, C compiler, `pkg-config`, the native test dependencies
(cJSON, SQLite, curl, OpenSSL and zlib development packages), Python 3 with
Pillow, Node 22 or newer, and Chromium. Altivec's bundled Font Awesome fonts
must be available under `/altivec/libs/cocoa/build-phone/Resources/Fonts`.
The runner searches PATH and `/opt/playwright-browsers`; select another browser
with `CHROMIUM=/path/to/chrome`. It installs or downloads nothing. Chromium is
launched with `--no-sandbox` for compatibility with this development container;
only the generated local fixture is opened. HTTP/HTTPS page requests are blocked.

## Workload and measurements

The five equally sized message groups are long text, text with three custom
emoji, photos, three-emoji-only messages, and stickers. Messages alternate
incoming/outgoing and include Japanese text, supplementary Unicode emoji,
parentheses, dollar signs, HTML-sensitive characters, and reactions. All 3,200
images (2,400 inline emoji, 400 photos, 400 stickers) must decode successfully.
There are 64 distinct 640×480 photo assets and 16 distinct 180×180 emoji/sticker
assets, all deterministic synthetic PNGs. Assets are reused, as in real chats;
this is not an animated-image or maximum image-memory benchmark.

The C driver uses the production schema and
`enil_account_chat_page_html`: SQLite paging, row construction, custom-emoji
formatting, full HTML generation and cleanup. Timings exclude fixture creation,
compilation, opening the database, and writing HTML. Two warmups precede seven
measured runs in one process. Database/OS caches are warm. The unavailable Cocoa
collation is replaced with byte ordering (chat queries do not use locale sorting),
and dates use the renderer's existing `strftime` fallback in UTC. Consequently,
Core Foundation locale/date formatting costs are not measured.

The browser uses the actual generated HTML/JavaScript and extracts current
`commonCSS` and `messageCSS` literals from `ENILUserDefaults.m`. Extraction fails
if those definitions disappear; update the extractor if CSS becomes dynamic.
It uses a 320×568 viewport, scale factor 2, and iPhone user agent. Workload v2
also verifies a 320-CSS-pixel layout: v1's simulated screen alone allowed legacy
pages without viewport metadata to lay out at 980 CSS pixels. The runner adds
viewport metadata to a separate browser fixture only when the rendered page
omits it. Existing viewport metadata and zoom styles are preserved. The raw
production HTML is kept unchanged. Each sample gets a new page, with browser caching disabled;
two browser warmups precede seven measured samples. Filesystem caches remain
warm. Measurements are:

- `native_render_ms`: production SQLite-to-HTML generation.
- `browser_load_ms`: navigation through load, font readiness, all image decodes,
  and two animation frames. This includes parsing, script execution and layout;
  it is a readiness proxy, not a GPU paint-duration measurement.
- `browser_scroll_ms`: 30 alternating top/bottom scrolls, forcing layout and
  waiting one animation frame each. This has a frame-rate floor; compare on the
  same host/display configuration.
- `browser_append_layout_ms`: production `appendMessage` plus forced layout for
  one cloned rendered text row.
- `browser_prepend_layout_ms`: production `prependMessages` plus forced layout
  for 40 cloned rendered text rows, after the append.

Message count, emoji/photo/sticker counts, decoded images, viewport width, and
insertion order are asserted before accepting a sample. Prepend scroll-anchor
drift is recorded separately in pixels; legacy zoomed pages can already have
drift, so this diagnostic is not a timing regression or a failed baseline.
Verify zero drift on the current renderer. Order and scroll-position checks run
outside the timed insertion regions. Results record individual samples,
median/min/max, compiler, browser, host, viewport, source revision, CSS hash and
document size. DOM node
count is captured after the 41 inserted rows; image counts describe the original
2,000-message page. Missing assets or incomplete rendering fail the run, rather
than producing an apparently faster result.

## Baseline and comparisons

The historical v1 baseline is [render-baseline.json](render-baseline.json).
Its layout width was not controlled, so it cannot be compared with v2 results.
It is retained for reference rather than overwritten with a faster baseline.

Use the current harness for both revisions. With the pre-change code in a
separate checkout (including its submodules), run the two versions sequentially:

```sh
make benchmark-render BENCH_ARGS='--source-root /path/to/before-checkout'
make benchmark-render BENCH_ARGS='--compare /path/to/before-checkout/build/render-benchmark/results.json'
```

`--source-root` selects production C sources and CSS from that checkout, writes
outputs beneath its `build/`, and records its Git revision. Both runs use the
invoking checkout's benchmark driver and browser checks. This avoids silently
comparing different harnesses when the benchmark itself changes.

Comparison is advisory by default because shared-runner load, browser updates,
CPU scheduling and host differences affect timings. Chromium on Linux is a
regression signal, not a speed prediction for Tiger WebKit or iOS UIWebView.
No device performance baseline is implied. The initial run had a roughly
26-second browser-load outlier despite a 542 ms median; raw samples are retained
to make that variability visible. Do not interpret small timing deltas on this
shared host as a confirmed regression.

On a controlled runner, add `--enforce --max-regression-percent 25` to fail when
any median exceeds the baseline by more than 25%. Enforcement refuses a baseline
with different recorded environment fields. Even matching fields cannot detect
all changes in host load or hardware configuration. Correctness failures always
fail regardless of `--enforce`. Workload name and message count must match the
baseline. Changing the fixture semantics requires a workload-version increment
and a fresh baseline; do not refresh baselines merely to hide a slowdown.

Outputs are in `build/render-benchmark/`: `results.json`, `browser.log`,
`fixture.sqlite`, compiled driver, CSS, fonts, assets, and `chats/chat.html`.
`chats/chat.html` is the untouched renderer output; `chats/browser.html` is the
viewport-normalized browser fixture. Results record whether metadata was
injected and the maximum prepend scroll-anchor drift. Open either HTML locally
to inspect it. To deliberately establish a new baseline after reviewing its
results:

```sh
make benchmark-render
cp build/render-benchmark/results.json docs/render-baseline.json
```

For a quick harness check, use `BENCH_ARGS='--messages 10 --repeats 3'`.
Message count must be a positive multiple of five; repeats must be at least three.
The benchmark is opt-in and separate from `make native-tests`; noisy performance
thresholds should not break ordinary correctness testing.

## Interactive scrolling preview

The same run also generates `build/render-benchmark/scroll-demo.html`. Open it
locally to exercise incoming text, outgoing text, photos, bursts and read markers
inside a 320×480 conversation. Its companion `chats/scroll-demo.html` uses the
production page plus a preview-only control script; the original `chat.html`
remains unchanged. Keep the generated directory together so assets resolve.
For a quick demonstration, run:

```sh
make benchmark-render BENCH_ARGS='--messages 40 --repeats 3'
```

The browser checks additionally verify animated append completion, eight-point
trailing clearance, incoming messages preserving a history reader's position,
burst following, and touch cancellation. Initial-load checks also simulate a
viewport height reported 30 points too tall, late layout growth, and a user
interrupting the deferred load correction. Append timing still measures synchronous
insertion/layout only, not the 500 ms animation. History prepend must cancel any
pending animation and preserve its scroll anchor. Short conversations retain
their existing top alignment; an offscreen entrance is not guaranteed there.
This Chromium preview does not establish iOS 6 or Tiger WebKit device behavior.
