/**************************************************************************/
/*!	\file	ats_manifest.h
	\brief	マニフェスト（.atsmanifest.yaml）の読み込み
***************************************************************************/
#ifndef ATS_MANIFEST_H
#define ATS_MANIFEST_H

#include "atomscript/ats_api.h"
#include "ats_diag.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ats {
namespace compiler {

// 型（enum は名前付き）
struct TypeRef {
	ats_type	base = ATS_TYPE_VOID;
	std::string	enumName;

	bool operator==( const TypeRef& o ) const { return base == o.base && enumName == o.enumName; }
	bool operator!=( const TypeRef& o ) const { return !(*this == o); }
	bool IsVoid() const		{ return base == ATS_TYPE_VOID; }
	std::string Name() const;
};

// マニフェストに書かれたリテラル（既定値・初期値）
struct Literal {
	bool		present = false;
	bool		quoted = false;
	std::string	text;
	int			line = 0;
};

struct MParam {
	std::string	name;
	std::string	display;
	TypeRef		type;
	Literal		def;
};

struct MCommand {
	std::string			name;
	std::string			display;
	std::string			category;
	std::string			description;
	bool				latent = false;
	bool				query = false;
	bool				deprecated = false;
	std::string			channel;
	std::vector<MParam>	params;
	TypeRef				ret;
	std::string			file;		// 定義されたマニフェストのファイル
	int					line = 0;

	uint32_t SignatureHash() const;
};

struct MEnum {
	std::string									name;
	std::string									display;
	std::vector<std::pair<std::string, int32_t>>	values;
	std::string									file;
	int											line = 0;
	bool Find( const std::string& v, int32_t* out ) const;
};

struct MVar {
	uint32_t	id = 0;
	std::string	bank;
	std::string	name;
	TypeRef		type;
	uint32_t	scope = ATS_SCOPE_PERSISTENT;
	Literal		init;
	std::string	file;
	int			line = 0;
};

struct MEvent {
	std::string			name;
	std::vector<MParam>	params;
	std::string			file;
	int					line = 0;
};

class Manifest {
public:
	std::string				project;
	std::vector<MEnum>		enums;
	std::vector<MVar>		vars;
	std::vector<MEvent>		events;
	std::vector<MCommand>	commands;		// コマンドとクエリ（query フラグで区別）
	std::vector<std::string> banks;
	uint64_t				hash = 0;		// 読み込んだファイル内容のハッシュ

	// path を読み込む（include も辿る）。失敗は diag に入る
	bool Load( const std::string& path, Diagnostics& diag );
	// 文字列から読み込む（テスト用。include は baseDir からの相対）
	bool LoadText( const std::string& text, const std::string& name, Diagnostics& diag, const std::string& baseDir = "" );

	const MEnum*	FindEnum( const std::string& name ) const;
	const MVar*		FindVar( const std::string& bank, const std::string& name ) const;
	const MVar*		FindVarById( uint32_t id ) const;
	const MEvent*	FindEvent( const std::string& name ) const;
	const MCommand*	FindCommand( const std::string& name ) const;
	bool			IsBank( const std::string& name ) const;

	// 型名（bool / int / float / string / handle / enum 名）を解決する
	bool			ResolveType( const std::string& name, TypeRef* out ) const;

private:
	bool LoadImpl( const std::string& text, const std::string& name, const std::string& baseDir,
				   Diagnostics& diag, int depth );
	std::vector<std::string> m_loaded;
};

// FNV-1a
uint32_t Fnv32( const std::string& s );
uint64_t Fnv64( const std::string& s, uint64_t h = 14695981039346656037ull );

}	// namespace compiler
}	// namespace ats

#endif	// ATS_MANIFEST_H
