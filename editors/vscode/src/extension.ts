/**************************************************************************/
/*	AtomScript VS Code 拡張
	言語サーバー（atsc lsp）を起動し、コンパイル・コマンド一覧の表示コマンドを提供する。
	構文解析・検査はすべて atsc が行う（拡張側に別の解析器は持たない）。
***************************************************************************/
import * as cp from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as vscode from 'vscode';
import { LanguageClient, LanguageClientOptions, ServerOptions } from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let output: vscode.OutputChannel;

//=========================================================================
// 設定・パス
//=========================================================================
function config() {
	return vscode.workspace.getConfiguration('atomscript');
}

// ワークスペースからの相対パスを絶対パスにする
function resolveSetting(value: string): string {
	if (!value || path.isAbsolute(value)) return value;
	const folder = vscode.workspace.workspaceFolders?.[0];
	return folder ? path.join(folder.uri.fsPath, value) : value;
}

// atsc の場所：設定 → 拡張に同梱 → PATH
function compilerPath(context: vscode.ExtensionContext): string {
	const configured = resolveSetting(config().get<string>('compilerPath', ''));
	if (configured) return configured;
	const exe = process.platform === 'win32' ? 'atsc.exe' : 'atsc';
	const bundled = context.asAbsolutePath(path.join('bin', exe));
	return fs.existsSync(bundled) ? bundled : exe;
}

// マニフェスト：設定 → スクリプトのフォルダから上へ探す（言語サーバーと同じ規則）
function findManifest(scriptPath: string): string | undefined {
	const configured = resolveSetting(config().get<string>('manifest', ''));
	if (configured) return configured;
	let dir = path.dirname(scriptPath);
	for (let i = 0; i < 32; ++i) {
		try {
			const found = fs.readdirSync(dir).filter(n => n.endsWith('.atsmanifest.yaml')).sort();
			if (found.length) return path.join(dir, found[0]);
		} catch {
			// 読めないフォルダは飛ばす
		}
		const parent = path.dirname(dir);
		if (parent === dir) break;
		dir = parent;
	}
	return undefined;
}

function run(exe: string, args: string[]): Promise<{ code: number; stdout: string; stderr: string }> {
	return new Promise(resolve => {
		cp.execFile(exe, args, { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 }, (err, stdout, stderr) => {
			const code = err ? (typeof (err as any).code === 'number' ? (err as any).code : -1) : 0;
			resolve({ code, stdout, stderr: stderr || (err && typeof (err as any).code !== 'number' ? String(err) : '') });
		});
	});
}

//=========================================================================
// 言語サーバー
//=========================================================================
async function startClient(context: vscode.ExtensionContext) {
	const exe = compilerPath(context);
	const serverOptions: ServerOptions = { command: exe, args: ['lsp'] };
	const clientOptions: LanguageClientOptions = {
		documentSelector: [{ scheme: 'file', language: 'atomscript' }],
		initializationOptions: { manifest: resolveSetting(config().get<string>('manifest', '')) },
		synchronize: {
			configurationSection: 'atomscript',
			// マニフェストが変わったら全スクリプトを検査し直す
			fileEvents: vscode.workspace.createFileSystemWatcher('**/*.atsmanifest.yaml'),
		},
		outputChannel: output,
	};
	client = new LanguageClient('atomscript', 'AtomScript', serverOptions, clientOptions);
	try {
		await client.start();
	} catch (e) {
		vscode.window.showErrorMessage(
			`AtomScript の言語サーバー（${exe}）を起動できません。設定 atomscript.compilerPath を確認してください。`);
		output.appendLine(String(e));
	}
}

async function stopClient() {
	if (client) {
		await client.stop();
		client = undefined;
	}
}

//=========================================================================
// コマンド
//=========================================================================
async function compileCurrent(context: vscode.ExtensionContext) {
	const editor = vscode.window.activeTextEditor;
	if (!editor || editor.document.languageId !== 'atomscript') {
		vscode.window.showWarningMessage('.ats ファイルを開いてから実行してください。');
		return;
	}
	await editor.document.save();
	const file = editor.document.uri.fsPath;
	const manifest = findManifest(file);
	if (!manifest) {
		vscode.window.showErrorMessage('マニフェスト（*.atsmanifest.yaml）が見つかりません。');
		return;
	}
	const r = await run(compilerPath(context), ['compile', file, '-m', manifest]);
	output.appendLine(`> atsc compile ${file} -m ${manifest}`);
	if (r.stdout) output.appendLine(r.stdout);
	if (r.stderr) output.appendLine(r.stderr);
	if (r.code === 0) {
		vscode.window.showInformationMessage(`コンパイルしました：${path.basename(file, '.ats')}.atsb`);
	} else {
		output.show(true);
		vscode.window.showErrorMessage('コンパイルに失敗しました。出力パネルを確認してください。');
	}
}

async function showCommandList(context: vscode.ExtensionContext) {
	const active = vscode.window.activeTextEditor?.document;
	const base = active?.uri.fsPath ?? vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
	let manifest = base ? findManifest(fs.existsSync(base) && fs.statSync(base).isDirectory() ? path.join(base, '_') : base) : undefined;
	if (!manifest) {
		const picked = await vscode.window.showOpenDialog({ filters: { Manifest: ['yaml'] }, canSelectMany: false });
		if (!picked?.length) return;
		manifest = picked[0].fsPath;
	}
	const html = path.join(os.tmpdir(), `atomscript-commands-${process.pid}.html`);
	const r = await run(compilerPath(context), ['gen', '-m', manifest, '--lang', 'html', '-o', html]);
	if (r.code !== 0) {
		output.appendLine(r.stderr);
		output.show(true);
		vscode.window.showErrorMessage('コマンド一覧を作れませんでした。マニフェストのエラーを確認してください。');
		return;
	}
	const panel = vscode.window.createWebviewPanel('atomscriptCommands', 'AtomScript コマンド一覧', vscode.ViewColumn.Beside, {});
	panel.webview.html = fs.readFileSync(html, 'utf8');
}

//=========================================================================
// 有効化
//=========================================================================
export async function activate(context: vscode.ExtensionContext) {
	output = vscode.window.createOutputChannel('AtomScript');
	context.subscriptions.push(
		output,
		vscode.commands.registerCommand('atomscript.compile', () => compileCurrent(context)),
		vscode.commands.registerCommand('atomscript.showCommandList', () => showCommandList(context)),
		vscode.commands.registerCommand('atomscript.restartServer', async () => {
			await stopClient();
			await startClient(context);
		}),
		vscode.workspace.onDidChangeConfiguration(async e => {
			// atsc の場所が変わったら起動し直す（マニフェストは言語サーバーに通知される）
			if (e.affectsConfiguration('atomscript.compilerPath')) {
				await stopClient();
				await startClient(context);
			}
		}),
	);
	await startClient(context);
}

export async function deactivate() {
	await stopClient();
}
