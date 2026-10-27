#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Native PDF rendering preserves font lookup and configuration diagnostics."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tmux_terminal import FakeResponses


BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('./snajpagent').resolve()
WITH_PDF = os.environ.get('SNAJPAGENT_TEST_WITH_PDF', '1') != '0'


def pdf_bytes():
    stream = b'BT /F1 18 Tf 40 120 Td (Native PDF font fixture) Tj ET\n'
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
               b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
               b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 360 180] '
               b'/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
               b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
               b'<< /Length ' + str(len(stream)).encode() + b' >>\nstream\n' + stream +
               b'endstream']
    result = bytearray(b'%PDF-1.4\n')
    offsets = []
    for number, obj in enumerate(objects, 1):
        offsets.append(len(result))
        result.extend(str(number).encode() + b' 0 obj\n' + obj + b'\nendobj\n')
    xref = len(result)
    result.extend(b'xref\n0 6\n0000000000 65535 f \n')
    for offset in offsets:
        result.extend(f'{offset:010d} 00000 n \n'.encode())
    result.extend(b'trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n' +
                  str(xref).encode() + b'\n%%EOF\n')
    return bytes(result)


def contains_image(value):
    if isinstance(value, dict):
        return value.get('type') == 'input_image' or any(
            contains_image(item) for item in value.values())
    return isinstance(value, list) and any(contains_image(item) for item in value)


@unittest.skipUnless(WITH_PDF, 'native PDF disabled in this build')
class NativePDF(unittest.TestCase):
    def render(self, setting=None, contents=None):
        with tempfile.TemporaryDirectory(prefix='snag-pdf-fonts-') as temporary:
            root = Path(temporary).resolve()
            state = root / 'state'
            state.mkdir(mode=0o700)
            document = root / 'sample.pdf'
            document.write_bytes(pdf_bytes())
            env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'config'),
                       XDG_CACHE_HOME=str(root / 'cache'), XDG_DATA_HOME=str(root / 'data'))
            for name in ('FONTCONFIG_FILE', 'FONTCONFIG_PATH', 'FONTCONFIG_SYSROOT'):
                env.pop(name, None)
            if contents is not None:
                (root / 'fonts.conf').write_text(contents)
            if setting == 'file':
                env['FONTCONFIG_FILE'] = str(root / 'fonts.conf')
            elif setting == 'path':
                env['FONTCONFIG_PATH'] = str(root)
            elif setting == 'empty':
                env.update(FONTCONFIG_FILE='', FONTCONFIG_PATH='')
            provider = FakeResponses()
            requests = []
            try:
                def respond(handler, request, sequence):
                    requests.append(request)
                    body = (provider.function_body(sequence, 'pdf-fonts', 'read_document',
                            {'path': str(document), 'first': 1, 'last': 1})
                            if len(requests) == 1 else
                            provider.response_body(sequence, 'PDF fixture finished'))
                    provider.reply(handler, body.encode(), close_header=True)
                provider.runtime_handler = respond
                config = state / 'config.ini'
                config.write_text('[agent]\nprovider=fixture\nmodel=host-model\n'
                                  'max_turn_retries=0\n[provider fixture]\n'
                                  f'base_url=http://127.0.0.1:{provider.port}\n'
                                  'api_key="irc-ui-secret"\nexact_token_count=false\n'
                                  '[ui]\ncolor=never\nmarkdown=false\npager=off\n')
                config.chmod(0o600)
                result = subprocess.run([str(BINARY), '--dotdir', str(state), '--no-listen',
                                         '--no-client', '-e', '--', 'Inspect the synthetic PDF'],
                                        env=env, capture_output=True, text=True, timeout=45)
                self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
                self.assertEqual(len(requests), 2)
                self.assertFalse(provider.failure, provider.failure)
                projected = requests[1]['input']
                pngs = [path.read_bytes() for path in (state / 'sessions').glob('*/media/*')
                        if path.is_file() and path.read_bytes().startswith(b'\x89PNG\r\n\x1a\n')]
                return result.stderr, projected, pngs
            finally:
                provider.close()

    def assert_rendered(self, projected, pngs):
        self.assertIn('Native PDF font fixture', json.dumps(projected))
        self.assertTrue(contains_image(projected), 'page image was not projected to the provider')
        self.assertTrue(pngs, 'rendered page was not retained')

    def test_default_fonts_without_configuration_error(self):
        settings = (None, 'empty') if sys.platform == 'darwin' else (None,)
        for setting in settings:
            with self.subTest(setting=setting):
                stderr, projected, pngs = self.render(setting)
                self.assertNotIn('Fontconfig error:', stderr)
                self.assert_rendered(projected, pngs)

    def test_explicit_missing_file_keeps_diagnostic_and_fallback(self):
        stderr, projected, pngs = self.render('file')
        self.assertIn('Fontconfig error:', stderr)
        self.assert_rendered(projected, pngs)

    def test_explicit_malformed_file_keeps_diagnostic_and_fallback(self):
        stderr, projected, pngs = self.render('file', '<fontconfig><broken>')
        self.assertIn('Fontconfig error:', stderr)
        self.assert_rendered(projected, pngs)

    def test_explicit_font_configuration_is_honored(self):
        contents = ('<fontconfig><dir>/no-snajpagent-font-fixture</dir>'
                    '<cachedir prefix="xdg">fontconfig</cachedir></fontconfig>')
        for setting in ('file', 'path'):
            with self.subTest(setting=setting):
                stderr, projected, pngs = self.render(setting, contents)
                self.assertNotIn('Fontconfig error:', stderr)
                self.assertIn('PDF page font unavailable or unusable', json.dumps(projected))
                self.assertFalse(contains_image(projected))
                self.assertFalse(pngs)


if __name__ == '__main__':
    unittest.main()
