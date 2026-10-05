#!/usr/bin/env python3
"""Synthetic incremental indexing and HTTP boundary proof."""
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import urlopen, Request
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'perf'))
from profile_server import Index, handler, CHUNK, MAX_LINE


def record(value):
    return ('KPERF '+json.dumps({'v': 1, 'type': 'metrics', 'stream': 'cpu',
                               'clock': 'monotonic_us', 'time': value,
                               'unit': 'us', 'aggregation': 'mean_per_frame',
                               'metrics': {'future.metric': value}})+'\n').encode()


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)/'logs'
        self.root.mkdir()
        self.log = self.root/'test.log'
        self.index = Index([self.root], Path(self.temp.name)/'cache.sqlite')

    def tearDown(self):
        self.index.db.close()
        self.temp.cleanup()

    def run_id(self):
        return self.index.runs()['runs'][0]['id']

    def test_partial_append_and_cache_reopen(self):
        line = record(12)
        self.log.write_bytes(line[:-4])
        run_id = self.run_id()
        result = self.index.run(run_id)
        self.assertFalse(result['complete'])
        self.assertEqual(result['total_records'], 0)
        with self.log.open('ab') as f:
            f.write(line[-4:])
        self.index.db.close()
        self.index = Index([self.root], Path(self.temp.name)/'cache.sqlite')
        result = self.index.run(run_id)
        self.assertTrue(result['complete'])
        self.assertEqual(result['records'][0]['metrics']['future.metric'], 12)
        self.assertEqual(result['total_records'], 1)
        self.assertEqual(self.index.run(run_id)['total_records'], 1)

    def test_rotation_and_inplace_rewrite(self):
        self.log.write_bytes(record(1)+record(2))
        run_id = self.run_id()
        first = self.index.run(run_id)
        self.log.rename(self.root/'old.bin')
        self.log.write_bytes(record(3))
        rotated = self.index.run(run_id)
        self.assertGreater(rotated['generation'], first['generation'])
        self.assertEqual([r['metrics']['future.metric'] for r in rotated['records']], [3])
        self.log.write_bytes(record(4)+record(5))
        rewritten = self.index.run(run_id)
        self.assertGreater(rewritten['generation'], rotated['generation'])
        self.assertEqual(rewritten['total_records'], 2)

    def test_pagination_export_and_issues(self):
        self.log.write_bytes(b''.join(record(i) for i in range(15))+b'KPERF {bad}\n')
        run_id = self.run_id()
        latest = self.index.run(run_id, 4)
        earlier = self.index.run(run_id, 4, latest['first_id'])
        self.assertEqual(len(latest['records']), 4)
        self.assertLess(earlier['last_id'], latest['first_id'])
        exported = [json.loads(line) for line in b''.join(self.index.export(run_id)).splitlines()]
        self.assertEqual(len(exported), 16)
        self.assertEqual([r['id'] for r in exported], list(range(1, 17)))
        self.assertTrue(any(i['name']=='malformed_record' for i in latest['issues']))
        self.assertIn('monotonic_us', latest['metadata']['time_domains'])
        self.assertEqual(self.index.run(run_id, 10000)['total_records'], 16)

    def test_bounded_chunks_and_oversized_lines(self):
        self.log.write_bytes(b'x'*(CHUNK+MAX_LINE)+b'\n'+record(9))
        summary = self.index.runs()['runs'][0]
        self.assertEqual(summary['indexed_bytes'], 0)
        self.assertEqual(summary['records'], 0)
        summary = self.index.run(summary['id'])
        self.assertEqual(summary['indexed_bytes'], CHUNK)
        self.assertFalse(summary['complete'])
        pending = self.index.db.execute('SELECT pending FROM runs').fetchone()[0]
        self.assertLessEqual(len(pending), MAX_LINE)
        result = self.index.run(summary['id'])
        self.assertTrue(result['complete'])
        self.assertEqual(result['issues'][0]['name'], 'oversized_record')
        self.assertEqual(result['total_records'], 2)

    def test_discovery_does_not_parse_and_export_loss(self):
        self.log.write_bytes(b''.join(record(i) for i in range(300)))
        summary = self.index.runs()['runs'][0]
        self.assertEqual(summary['records'], 0)
        self.assertEqual(summary['indexed_bytes'], 0)
        self.assertEqual(self.index.runs()['runs'][0]['indexed_bytes'], 0)
        export = self.index.export(summary['id'])
        next(export)
        self.log.rename(self.root/'old.bin')
        self.log.write_bytes(record(999))
        self.index.run(summary['id'])
        remaining = [json.loads(line) for line in export]
        self.assertEqual(remaining[-1]['name'], 'export_incomplete')
        self.assertEqual(remaining[-1]['count'], 44)

    def test_discovery_ttl_and_selected_summary(self):
        now = [0]
        self.index.clock = lambda: now[0]
        self.log.write_bytes(record(1))
        first = self.index.runs()['runs'][0]
        (self.root/'later.log').write_bytes(record(2))
        (self.root/'unrelated.txt').write_text('irrelevant')
        self.assertEqual(len(self.index.runs()['runs']), 1)
        self.index.run(first['id'])
        self.assertEqual(self.index.runs()['runs'][0]['records'], 1)
        now[0] = 11
        self.assertEqual(len(self.index.runs()['runs']), 2)
        self.assertEqual(sum(r['records'] for r in self.index.runs()['runs']), 1)

    def test_run_listing_pages_search_and_selected(self):
        for i in range(125):
            (self.root/f'Alpha-{i:03d}.log').write_bytes(record(i))
        first = self.index.runs()
        self.assertEqual((len(first['runs']), first['total'], first['more']), (100, 125, True))
        second = self.index.runs(limit=30, offset=100)
        self.assertEqual((len(second['runs']), second['page_count'], second['more']), (25, 25, False))
        selected = [second['runs'][0]['id'], first['runs'][0]['id'], second['runs'][0]['id']]
        page = self.index.runs(limit=10, selected=selected)
        self.assertEqual(len(page['runs']), 11)
        self.assertEqual(len({r['id'] for r in page['runs']}), 11)
        search = self.index.runs(q='aLPHa-12', selected=[first['runs'][0]['id']])
        self.assertEqual(search['total'], 5)
        self.assertEqual(search['page_count'], 5)
        self.assertTrue(any(r['id'] == first['runs'][0]['id'] for r in search['runs']))
        self.assertTrue(all(r['indexed_bytes'] == 0 for r in page['runs']))
        self.assertEqual(self.index.runs(limit=10000)['limit'], 500)
        self.assertEqual(self.index.runs(offset=-10)['offset'], 0)

    def test_conditional_listing_and_run_http(self):
        self.log.write_bytes(record(7))
        run_id = self.run_id()
        server = ThreadingHTTPServer(('127.0.0.1', 0), handler(self.index))
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        base = f'http://127.0.0.1:{server.server_port}'
        try:
            urls = ['/api/runs?limit=1&offset=0&q=TEST&selected='+run_id, '/api/run?id='+run_id]
            for url in urls:
                with urlopen(base+url) as response:
                    first_etag = response.headers['ETag']
                    self.assertTrue(response.read())
                with self.assertRaises(HTTPError) as error:
                    urlopen(Request(base+url, headers={'If-None-Match': first_etag}))
                self.assertEqual(error.exception.code, 304)
                self.assertEqual(error.exception.read(), b'')
                self.assertEqual(error.exception.headers['ETag'], first_etag)
                error.exception.close()
                with self.log.open('ab') as capture:
                    capture.write(record(8))
                if url.startswith('/api/runs'):
                    self.index.run(run_id)  # Listing reflects selected cached index changes.
                with urlopen(Request(base+url, headers={'If-None-Match': first_etag})) as response:
                    self.assertEqual(response.status, 200)
                    self.assertNotEqual(response.headers['ETag'], first_etag)
        finally:
            server.shutdown(); thread.join(); server.server_close()

    def test_path_confinement_and_http(self):
        outside = Path(self.temp.name)/'secret.log'
        outside.write_bytes(record(99))
        (self.root/'escape.log').symlink_to(outside)
        self.log.write_bytes(record(7))
        self.assertEqual(len(self.index.runs()['runs']), 1)
        run_id = self.run_id()
        with self.assertRaises(KeyError):
            self.index.run('../../secret.log')
        server = ThreadingHTTPServer(('127.0.0.1', 0), handler(self.index))
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        base = f'http://127.0.0.1:{server.server_port}'
        try:
            with urlopen(base+'/api/run?id='+run_id) as response:
                self.assertEqual(json.load(response)['total_records'], 1)
            with urlopen(base+'/api/export?id='+run_id) as response:
                self.assertEqual(len(response.read().splitlines()), 1)
            for path in ['/api/run?id=unknown', '/api/source?id='+run_id, '/../../secret.log']:
                with self.assertRaises(HTTPError) as error:
                    urlopen(base+path)
                self.assertEqual(error.exception.code, 404)
                error.exception.close()
            self.log.unlink()
            self.log.symlink_to(outside)
            with self.assertRaises(KeyError):
                self.index.run(run_id)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == '__main__':
    unittest.main()
