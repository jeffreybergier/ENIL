"""Exercise history, pagination, and live photo markup through the C renderer."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class ChatLayoutTests(unittest.TestCase):
    def test_independent_messages(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / 'chat-layout')
            shared = REPO / 'source/shared'
            flags = shlex.split(subprocess.check_output(
                ['pkg-config', '--cflags', '--libs', 'libcjson'], text=True))
            subprocess.run([
                'cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-I' + str(shared),
                str(REPO / 'source/tests/native_chat_layout.c'),
                str(shared / 'enil_html.c'), str(shared / 'enil_strbuf.c'),
                *flags, '-o', binary], check=True)
            subprocess.run([binary], check=True)
