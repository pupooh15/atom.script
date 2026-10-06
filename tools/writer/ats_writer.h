/**************************************************************************/
/*!	\file	ats_writer.h
	\brief	.atsb の書き出し（コンパイラ・テスト用）
	\note
	ツール側のコードなので STL を使う。ランタイムには含めない。
***************************************************************************/
#ifndef ATS_WRITER_H
#define ATS_WRITER_H

#include "atomscript/ats_api.h"
#include "atomscript/ats_format.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ats {
namespace writer {

struct Label { int id = -1; };

class ProgramWriter {
public:
	explicit ProgramWriter( const std::string& script_id );

	// テーブル -------------------------------------------------------------
	uint32_t	Str( const std::string& s );
	uint32_t	Const( uint64_t v );
	uint16_t	Command( const std::string& name, const std::vector<ats_type>& params,
						 ats_type ret = ATS_TYPE_VOID, uint32_t sig_hash = 0 );
	uint16_t	Query( const std::string& name, const std::vector<ats_type>& params,
					   ats_type ret, uint32_t sig_hash = 0 );
	uint16_t	SharedVar( uint32_t id, ats_type type );
	uint16_t	ScriptVar( const std::string& name, ats_type type, uint64_t init = 0,
						   bool transient = false, const std::string& was = "" );
	uint16_t	ScriptVarStr( const std::string& name, const std::string& init,
							  bool transient = false, const std::string& was = "" );
	void		SetManifestHash( uint64_t h ) { m_manifestHash = h; }

	// エントリ（イベント・関数）--------------------------------------------
	// 関数は呼び出し前に宣言だけしておける（DeclareFunction → BeginEntry）
	uint16_t	DeclareFunction( const std::string& name, const std::vector<ats_type>& params,
								 ats_type ret, uint16_t localc );
	uint16_t	BeginEvent( const std::string& name, const std::vector<ats_type>& params, uint16_t localc );
	void		BeginFunction( uint16_t fn );
	void		EndEntry();
	// ローカル数は本体を生成した後で確定する（コンパイラ用）
	void		SetLocalCount( uint16_t entry, uint16_t localc ) { m_entries[entry].e.localc = localc; }

	// 命令 -------------------------------------------------------------------
	void		Op( fmt::Op op );
	void		PushI( int32_t v );
	void		PushF( float v );
	void		PushK( uint64_t v );
	void		PushStr( const std::string& s );
	void		LdLocal( uint16_t i );
	void		StLocal( uint16_t i );
	void		LdVar( uint16_t i );
	void		StVar( uint16_t i );
	void		CallCmd( uint16_t imp );
	void		CallQuery( uint16_t imp );
	void		CallFn( uint16_t fn );
	void		Fire( const std::string& event, uint8_t argc );

	Label		NewLabel();
	void		Bind( Label l );
	void		Jmp( Label l );
	void		Jz( Label l );
	void		Jnz( Label l );
	void		Fork( Label l );
	void		Switch( const std::vector<std::pair<int32_t, Label>>& cases, Label def );

	// 現在位置に行番号（と @node の ID）を付ける
	void		Line( uint32_t line, const std::string& node = "" );

	uint32_t	CodePos() const { return (uint32_t)m_code.size(); }

	// 書き出し。max_stack は制御フローから自動で求める
	// 失敗した場合は空を返し、error に理由を入れる
	std::vector<uint8_t> Build( std::string* error = nullptr );

	// テスト用：生のバイト列を書く
	void		Raw( const std::vector<uint8_t>& bytes );
	// テスト用：max_stack を解析せず固定値にする（不正なコードを VM の検証器に通すため）
	void		SetFixedMaxStack( uint16_t v ) { m_fixedMaxStack = v; }

private:
	struct Fixup { uint32_t pos; uint32_t base; int label; };
	struct Entry { fmt::EntryEntry e; bool defined; };

	void		U8( uint8_t v )   { m_code.push_back( v ); }
	void		U16( uint16_t v ) { Put( &v, 2 ); }
	void		U32( uint32_t v ) { Put( &v, 4 ); }
	void		Put( const void* p, size_t n );
	void		JumpOp( fmt::Op op, Label l );
	bool		ComputeMaxStack( fmt::EntryEntry& e, std::string* error ) const;

	std::string						m_scriptId;
	uint64_t						m_manifestHash = 0;
	std::vector<std::string>		m_strings;
	std::vector<uint64_t>			m_consts;
	std::vector<fmt::ImportEntry>	m_imports;
	std::vector<fmt::VarEntry>		m_vars;
	std::vector<Entry>				m_entries;
	std::vector<fmt::DebugEntry>	m_debug;
	std::vector<uint8_t>			m_code;
	std::vector<int64_t>			m_labels;	// -1 なら未確定
	std::vector<Fixup>				m_fixups;
	int								m_current = -1;
	int								m_fixedMaxStack = -1;
};

}	// namespace writer
}	// namespace ats

#endif	// ATS_WRITER_H
