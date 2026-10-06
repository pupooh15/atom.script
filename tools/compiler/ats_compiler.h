/**************************************************************************/
/*!	\file	ats_compiler.h
	\brief	.ats → .atsb のコンパイラ
	\note
	CLI（atsc）、Unity のインポーター、UE の Factory、将来のノードエディタが
	すべてこの 1 つの実装を使う。
***************************************************************************/
#ifndef ATS_COMPILER_H
#define ATS_COMPILER_H

#include "ats_diag.h"
#include "ats_manifest.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ats {
namespace compiler {

struct CompileOptions {
	bool	debugInfo = true;	// 行番号と @node の ID を DBUG セクションに入れる
};

struct CompileResult {
	std::string				scriptId;
	std::vector<uint8_t>	bytes;		// .atsb（エラーがあれば空）
};

// 1 ファイルをコンパイルする。エラー・警告は diag に追加される
bool CompileSource( const std::string& source, const std::string& file, const Manifest& manifest,
					Diagnostics& diag, CompileResult* out, const CompileOptions& options = CompileOptions() );

// .atsb を人が読める形にする（atsc disasm）
bool Disassemble( const std::vector<uint8_t>& bytes, std::string* out, std::string* error );

//=========================================================================
// マニフェストからのコード生成（atsc gen）
//=========================================================================
struct GenOptions {
	std::string	nameSpace;		// 空ならマニフェストの project（識別子に使えない文字は _ に置き換える）
	std::string	source;			// 生成元のファイル名（ヘッダーのコメント用）
};

// C++ のヘッダー（ヘッダーのみで完結）。コマンド・クエリの実装インターフェイスと登録関数、
// enum、共有変数の ID、イベントの発火関数を含む
std::string GenerateCpp( const Manifest& manifest, const GenOptions& options );

// プランナー向けのコマンド一覧（HTML）
std::string GenerateHtml( const Manifest& manifest, const GenOptions& options );

}	// namespace compiler
}	// namespace ats

#endif	// ATS_COMPILER_H
