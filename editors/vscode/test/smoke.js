// atsc lsp を実際に起動し、標準入出力（Content-Length 区切り）でやり取りできるか確かめる。
//   node test/smoke.js [atsc のパス]
// 省略時は bin/ に同梱したもの、なければ ../../build/Release/atsc(.exe)
'use strict';
const cp = require('child_process');
const fs = require('fs');
const path = require('path');

const exe = process.platform === 'win32' ? 'atsc.exe' : 'atsc';
const atsc = process.argv[2]
	|| [path.join(__dirname, '..', 'bin', exe), path.join(__dirname, '..', '..', '..', 'build', 'Release', exe)].find(p => fs.existsSync(p));
if (!atsc) { console.error('atsc が見つかりません'); process.exit(2); }

const samples = path.resolve(__dirname, '..', '..', '..', 'samples');
const toUri = p => 'file:///' + p.replace(/\\/g, '/').replace(/^([A-Za-z]):/, (m, d) => d.toLowerCase() + '%3A');

const server = cp.spawn(atsc, ['lsp'], { stdio: ['pipe', 'pipe', 'inherit'] });
let buffer = Buffer.alloc(0);
const waiters = [];
const received = [];

server.stdout.on('data', chunk => {
	buffer = Buffer.concat([buffer, chunk]);
	for (;;) {
		const sep = buffer.indexOf('\r\n\r\n');
		if (sep < 0) return;
		const m = /Content-Length: (\d+)/i.exec(buffer.slice(0, sep).toString());
		const len = Number(m[1]);
		if (buffer.length < sep + 4 + len) return;
		const msg = JSON.parse(buffer.slice(sep + 4, sep + 4 + len).toString('utf8'));
		buffer = buffer.slice(sep + 4 + len);
		received.push(msg);
		for (const w of waiters.splice(0)) w();
	}
});

function send(msg) {
	const body = Buffer.from(JSON.stringify(Object.assign({ jsonrpc: '2.0' }, msg)), 'utf8');
	server.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
	server.stdin.write(body);
}

function waitFor(pred, ms = 5000) {
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => reject(new Error('タイムアウト')), ms);
		const check = () => {
			const i = received.findIndex(pred);
			if (i >= 0) { clearTimeout(timer); resolve(received.splice(i, 1)[0]); }
			else waiters.push(check);
		};
		check();
	});
}

function assert(cond, msg) {
	if (!cond) { console.error('NG: ' + msg); process.exitCode = 1; }
	else console.log('OK: ' + msg);
}

(async () => {
	send({ id: 1, method: 'initialize', params: { rootUri: toUri(samples), capabilities: {} } });
	const init = await waitFor(m => m.id === 1);
	assert(init.result.capabilities.completionProvider, 'initialize に補完の能力が入っている');
	send({ method: 'initialized', params: {} });

	// サンプルはエラーなし（マニフェストはフォルダから見つける）
	const merchant = path.join(samples, 'merchant.ats');
	const uri = toUri(merchant);
	send({ method: 'textDocument/didOpen', params: { textDocument: { uri, languageId: 'atomscript', version: 1, text: fs.readFileSync(merchant, 'utf8') } } });
	const d1 = await waitFor(m => m.method === 'textDocument/publishDiagnostics' && m.params.uri === uri);
	assert(d1.params.diagnostics.length === 0, 'merchant.ats に診断がない');

	// 壊すとエラーが出る（日本語のメッセージが壊れずに届く）
	send({ method: 'textDocument/didChange', params: { textDocument: { uri, version: 2 }, contentChanges: [{ text: 'script "x/y"\nevent OnTalk(target: handle) {\n  ShowMessage("あ")\n}\n' }] } });
	const d2 = await waitFor(m => m.method === 'textDocument/publishDiagnostics' && m.params.uri === uri);
	assert(d2.params.diagnostics.length === 1 && d2.params.diagnostics[0].message.includes('await が必要'), 'await の付け忘れが報告される');

	send({ id: 2, method: 'textDocument/completion', params: { textDocument: { uri }, position: { line: 2, character: 2 } } });
	const c = await waitFor(m => m.id === 2);
	assert(c.result.some(i => i.label === 'ShowMessage'), '補完にコマンドが出る');

	send({ id: 3, method: 'shutdown' });
	await waitFor(m => m.id === 3);
	send({ method: 'exit' });
	server.on('exit', code => {
		assert(code === 0, 'shutdown → exit で終了コード 0');
		if (process.exitCode) console.error('\n失敗あり'); else console.log('\nすべて成功');
	});
})().catch(e => { console.error(e); process.exit(1); });
