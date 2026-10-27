#!/usr/bin/env python3
"""Local protocol fixture for seekdb SQL end-to-end tests. No real AI keys."""
import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class Handler(BaseHTTPRequestHandler):
    lock = threading.Lock()
    requests = []
    def log_message(self, *args):
        pass
    def respond(self, code, value):
        data = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        with self.lock:
            self.respond(200, list(self.requests))
    def do_POST(self):
        try:
            size = int(self.headers.get('Content-Length', 0))
            if size <= 0 or size > 16 * 1024 * 1024:
                return self.respond(400, {'error': 'invalid request size'})
            body = json.loads(self.rfile.read(size))
            with self.lock:
                self.requests.append({'path': self.path, 'body': body})
            if self.path.endswith('/embeddings') or self.path.endswith('/api/embed'):
                inputs = body.get('input', [])
                if isinstance(inputs, str):
                    inputs = [inputs]
                dimension = body.get('dimensions', 768)
                if not isinstance(dimension, int) or not 0 < dimension <= 8192:
                    return self.respond(400, {'error': 'invalid dimensions'})
                vectors = []
                for text in inputs:
                    digest = hashlib.sha256(text.encode()).digest()
                    vectors.append([round(digest[i % len(digest)] / 255, 6) for i in range(dimension)])
                if '/wrong-dimension/' in self.path:
                    vectors = [v[:-1] for v in vectors]
                if '/invalid-element/' in self.path:
                    vectors[0][0] = None
                if self.path.endswith('/api/embed'):
                    return self.respond(200, {'embeddings': vectors})
                data = [{'index': i, 'embedding': v} for i, v in enumerate(vectors)]
                data.reverse()  # Exercise the index mapping rather than arrival order.
                if '/duplicate-index/' in self.path and len(data) > 1:
                    data[0]['index'] = data[1]['index']
                return self.respond(200, {'data': data})
            if self.path.endswith('/rerank'):
                query = body.get('query', '').casefold()
                documents = body.get('documents', [])
                results = [{'index': i, 'relevance_score': float(text.casefold().count(query)) if query else 0.0}
                           for i, text in enumerate(documents)]
                results.sort(key=lambda item: (item['relevance_score'], item['index']), reverse=True)
                if '/invalid-index/' in self.path and results:
                    results[0]['index'] = len(results)
                return self.respond(200, {'results': results})
            if self.path.endswith('/api/generate'):
                if body.get('stream') is not False:
                    return self.respond(400, {'error': 'stream=false is required'})
                return self.respond(200, {'response': body.get('prompt', '')})
            if self.path.endswith('/chat/completions'):
                messages = body.get('messages', [])
                text = messages[-1].get('content', '') if messages else ''
                if '/empty/' in self.path:
                    text = ''
                if '/wrong-type/' in self.path:
                    text = {'unexpected': True}
                return self.respond(200, {'choices': [{'message': {'content': text}}]})
            self.respond(404, {'error': 'unknown fixture route'})
        except (ValueError, TypeError, KeyError, AttributeError) as error:
            self.respond(400, {'error': str(error)})

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8000)
    arguments = parser.parse_args()
    ThreadingHTTPServer(('127.0.0.1', arguments.port), Handler).serve_forever()
