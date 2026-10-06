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

}	// namespace compiler
}	// namespace ats

#endif	// ATS_COMPILER_H
