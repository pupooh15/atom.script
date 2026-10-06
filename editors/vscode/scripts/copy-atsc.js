// ビルド済みの atsc を拡張の bin/ にコピーする（vsix に同梱するため）。
//   node scripts/copy-atsc.js [atsc のパス]
// 省略時は ../../build/Release/atsc(.exe)
'use strict';
const fs = require('fs');
const path = require('path');

const exe = process.platform === 'win32' ? 'atsc.exe' : 'atsc';
const src = process.argv[2] || path.join(__dirname, '..', '..', '..', 'build', 'Release', exe);
if (!fs.existsSync(src)) {
	console.error(`${src} がありません。先に CMake の Release ビルドで atsc を作ってください。`);
	process.exit(1);
}
const dstDir = path.join(__dirname, '..', 'bin');
fs.mkdirSync(dstDir, { recursive: true });
fs.copyFileSync(src, path.join(dstDir, exe));
console.log(`copied ${src} -> bin/${exe}`);
