"""Linux-only offline rendering and text preservation, using production C code."""
from contextlib import closing
from html.parser import HTMLParser
from pathlib import Path
import json
import os
import shlex
import sqlite3
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class Body(HTMLParser):
    def __init__(self, html):
        super().__init__(convert_charrefs=True)
        self.text = ''
        self.images = []
        self.feed(html)

    def handle_data(self, text):
        self.text += text

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'br':
            self.text += '\n'
        elif tag == 'img':
            self.images.append(attrs)
            self.text += attrs['alt']
        else:
            raise AssertionError('Unexpected HTML tag: ' + tag)


class MessageFormatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'render'
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'libcjson', 'sqlite3', 'libcurl'], text=True))
        shared = REPO / 'source/shared'
        subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-I' + str(shared),
                        str(REPO / 'source/tests/native_message_format.c'),
                        *[str(shared / f) for f in ['enil_message_format.c',
                          'enil_db.c', 'enil_strbuf.c', 'enil_line.c']],
                        '-Wl,--gc-sections', *flags, '-o', str(cls.binary)], check=True)

    def render(self, path, message='*'):
        data = subprocess.check_output([str(self.binary), str(path), message])
        fields = data.decode().split('\0')[:-1]
        return dict(zip(fields[::2], fields[1::2]))

    def fixture(self, text, spans, image=True, message='m'):
        path = Path(self.temp.name) / 'fixture.sqlite'
        if path.exists():
            path.unlink()
        with closing(sqlite3.connect(path)) as db, db:
            db.executescript('''
                CREATE TABLE messages_v2(id TEXT, text TEXT, replace_json TEXT);
                CREATE TABLE message_sticons(message_id TEXT, ordinal INTEGER,
                                            package_id TEXT, sticon_id TEXT);
                CREATE TABLE sticons_v2(package_id TEXT, sticon_id TEXT,
                    image_path TEXT, image_width INTEGER, image_height INTEGER);
                CREATE TABLE stickers_v2(package_id TEXT, sticker_id TEXT,
                    image_path TEXT, image_width INTEGER, image_height INTEGER);
            ''')
            resources = [dict(S=s, E=e, productId='pkg', sticonId='1') for s, e in spans]
            db.execute('INSERT INTO messages_v2 VALUES(?,?,?)',
                       (message, text, json.dumps({'sticon': {'resources': resources}})))
            for i in range(len(spans)):
                db.execute('INSERT INTO message_sticons VALUES(?,?,?,?)', (message, i, 'pkg', '1'))
            db.execute('INSERT INTO sticons_v2 VALUES(?,?,?,?,?)',
                       ('pkg', '1', 'emoji.png' if image else '', 100, 100))
        body = Body(self.render(path, message)[message])
        self.assertEqual(body.text, text)
        return body

    def test_regression_and_ordinary_punctuation(self):
        text = 'Images failed :( \n\nI will fix it tonight (shy)'
        body = self.fixture(text, [(text.index('(shy)'), len(text))])
        self.assertEqual([i['alt'] for i in body.images], ['(shy)'])
        self.assertEqual(len(self.fixture('(ordinary) $5 $ (shy)', [(16, 21)]).images), 1)

    def test_unicode_and_legacy_bytes(self):
        text = '日本😀 (shy)'
        for span in [(5, 10), (11, 16)]:
            self.assertEqual(len(self.fixture(text, [span]).images), 1)

    def test_multiple_and_jumbo(self):
        body = self.fixture('(shy) $', [(0, 5), (6, 7)])
        self.assertEqual(len(body.images), 2)
        self.assertEqual(body.images[0]['width'], '33')
        body = self.fixture('a (shy)', [(2, 7)])
        self.assertNotEqual(body.images[0]['width'], '33')

    def test_invalid_metadata_and_missing_images(self):
        for spans in [[(-1, 5)], [(0, 999)], [(0.5, 5)], [(None, 5)],
                      [(0, 5), (0, 5)], [(6, 11), (0, 5)], [], [(2, 3)]]:
            self.assertFalse(self.fixture('(shy) (shy)', spans).images)
        self.assertFalse(self.fixture('😀(shy)', [(1, 7)]).images)
        self.assertFalse(self.fixture('(shy)', [(0, 5)], image=False).images)

    def test_html_escaping(self):
        self.assertEqual(len(self.fixture('<script> & " (shy)', [(13, 18)]).images), 1)
        self.assertEqual(len(self.fixture('("<&)', [(0, 5)]).images), 1)

    def test_preview(self):
        body = self.fixture(':( ordinary (note) 日本 (shy)', [], message='preview')
        self.assertEqual([i['alt'] for i in body.images], ['(shy)'])

    @unittest.skipUnless(os.environ.get('ENIL_RENDER_TEST_DB'), 'optional private local database')
    def test_local_database(self):
        path = Path(os.environ['ENIL_RENDER_TEST_DB']).resolve()
        with closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)) as db:
            originals = dict(db.execute('SELECT id,coalesce(text,\'\') FROM messages_v2'))
        rendered = self.render(path)
        self.assertEqual(set(originals), set(rendered))
        images = 0
        for key, html in rendered.items():
            body = Body(html)
            self.assertEqual(body.text, originals[key], key)
            images += len(body.images)
        print(f'Offline corpus: {len(rendered)} messages; {images} custom emoji; all text preserved')


if __name__ == '__main__':
    unittest.main()
