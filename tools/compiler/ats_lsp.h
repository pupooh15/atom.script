/**************************************************************************/
/*!	\file	ats_lsp.h
	\brief	言語サーバー（atsc lsp）
	\note
	VS Code 拡張などから標準入出力で使う。構文解析・意味検査はコンパイラと同じ実装を使う。
	入出力から切り離してあり、テストでは Handle() に JSON を渡して結果を確かめる。
***************************************************************************/
#ifndef ATS_LSP_H
#define ATS_LSP_H

#include <memory>
#include <string>
#include <vector>

namespace ats {
namespace compiler {

class LanguageServer {
public:
	LanguageServer();
	~LanguageServer();

	// 受け取ったメッセージ（JSON-RPC 1 件）を処理し、送り返すメッセージを返す
	std::vector<std::string> Handle( const std::string& message );

	// exit を受け取ったら true
	bool ShouldExit() const;
	// shutdown を受け取ってから exit したか（終了コードの判断に使う）
	bool ShutdownRequested() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// 標準入出力で言語サーバーを動かす（atsc lsp）。終了コードを返す
int RunLanguageServer();

}	// namespace compiler
}	// namespace ats

#endif	// ATS_LSP_H
