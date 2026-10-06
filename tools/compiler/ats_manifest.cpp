/**************************************************************************/
/*!	\file	ats_manifest.cpp
	\brief	マニフェスト（.atsmanifest.yaml）の読み込み
***************************************************************************/
#include "ats_manifest.h"
#include "ats_yaml.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ats {
namespace compiler {

using yaml::Node;

//=========================================================================
// 補助
//=========================================================================
uint32_t Fnv32( const std::string& s )
{
	uint32_t h = 2166136261u;
	for( unsigned char c : s ){ h ^= c; h *= 16777619u; }
	return h;
}

uint64_t Fnv64( const std::string& s, uint64_t h )
{
	for( unsigned char c : s ){ h ^= c; h *= 1099511628211ull; }
	return h;
}

std::string TypeRef::Name() const
{
	switch( base ){
	case ATS_TYPE_VOID:		return "void";
	case ATS_TYPE_BOOL:		return "bool";
	case ATS_TYPE_INT:		return "int";
	case ATS_TYPE_FLOAT:	return "float";
	case ATS_TYPE_STRING:	return "string";
	case ATS_TYPE_ENUM:		return enumName;
	case ATS_TYPE_HANDLE:	return "handle";
	}
	return "?";
}

uint32_t MCommand::SignatureHash() const
{
	// 例："ShowMessage(string,Face)->void"。生成コードも同じ規則で計算する
	std::string s = name + "(";
	for( size_t i = 0; i < params.size(); ++i ){
		if( i ) s += ",";
		s += params[i].type.Name();
	}
	s += ")->" + ret.Name();
	uint32_t h = Fnv32( s );
	return h ? h : 1;
}

bool MEnum::Find( const std::string& v, int32_t* out ) const
{
	for( const auto& kv : values ) if( kv.first == v ){ *out = kv.second; return true; }
	return false;
}

static bool ParseInt( const std::string& s, long long* out )
{
	if( s.empty() ) return false;
	char* end = nullptr;
	long long v = std::strtoll( s.c_str(), &end, 0 );
	if( *end ) return false;
	*out = v;
	return true;
}

static bool ParseBool( const std::string& s, bool* out )
{
	if( s == "true" ){ *out = true; return true; }
	if( s == "false" ){ *out = false; return true; }
	return false;
}

static Literal ToLiteral( const Node* n )
{
	Literal l;
	if( !n || n->IsNull() ) return l;
	l.present = true;
	l.quoted  = n->quoted;
	l.text    = n->scalar;
	l.line    = n->line;
	return l;
}

//=========================================================================
// 検索
//=========================================================================
const MEnum* Manifest::FindEnum( const std::string& name ) const
{
	for( const MEnum& e : enums ) if( e.name == name ) return &e;
	return nullptr;
}

const MVar* Manifest::FindVar( const std::string& bank, const std::string& name ) const
{
	for( const MVar& v : vars ) if( v.bank == bank && v.name == name ) return &v;
	return nullptr;
}

const MVar* Manifest::FindVarById( uint32_t id ) const
{
	for( const MVar& v : vars ) if( v.id == id ) return &v;
	return nullptr;
}

const MEvent* Manifest::FindEvent( const std::string& name ) const
{
	for( const MEvent& e : events ) if( e.name == name ) return &e;
	return nullptr;
}

const MCommand* Manifest::FindCommand( const std::string& name ) const
{
	for( const MCommand& c : commands ) if( c.name == name ) return &c;
	return nullptr;
}

bool Manifest::IsBank( const std::string& name ) const
{
	for( const std::string& b : banks ) if( b == name ) return true;
	return false;
}

bool Manifest::ResolveType( const std::string& name, TypeRef* out ) const
{
	*out = TypeRef();
	if( name == "bool" )	{ out->base = ATS_TYPE_BOOL; return true; }
	if( name == "int" )		{ out->base = ATS_TYPE_INT; return true; }
	if( name == "float" )	{ out->base = ATS_TYPE_FLOAT; return true; }
	if( name == "string" )	{ out->base = ATS_TYPE_STRING; return true; }
	if( name == "handle" )	{ out->base = ATS_TYPE_HANDLE; return true; }
	if( FindEnum( name ) )	{ out->base = ATS_TYPE_ENUM; out->enumName = name; return true; }
	return false;
}

//=========================================================================
// 読み込み
//=========================================================================
static bool ReadFile( const std::string& path, std::string* out )
{
	std::ifstream f( path, std::ios::binary );
	if( !f ) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	*out = ss.str();
	return true;
}

static std::string DirOf( const std::string& path )
{
	size_t p = path.find_last_of( "/\\" );
	return p == std::string::npos ? "" : path.substr( 0, p + 1 );
}

bool Manifest::Load( const std::string& path, Diagnostics& diag )
{
	std::string text;
	if( !ReadFile( path, &text ) ){
		diag.Error( path, 0, 0, "マニフェストを開けません" );
		return false;
	}
	m_loaded.push_back( path );
	return LoadImpl( text, path, DirOf( path ), diag, 0 );
}

bool Manifest::LoadText( const std::string& text, const std::string& name, Diagnostics& diag, const std::string& baseDir )
{
	return LoadImpl( text, name, baseDir, diag, 0 );
}

bool Manifest::LoadImpl( const std::string& text, const std::string& file, const std::string& baseDir,
						 Diagnostics& diag, int depth )
{
	const size_t errorsBefore = diag.ErrorCount();
	auto err = [&]( int line, const std::string& msg ) { diag.Error( file, line, 1, msg ); };

	hash = Fnv64( text, hash ? hash : 14695981039346656037ull );

	Node root;
	std::string yerr;
	if( !yaml::Parse( text, &root, &yerr ) ){
		int line = std::atoi( yerr.c_str() );
		size_t c = yerr.find( ": " );
		err( line, "YAML: " + (c == std::string::npos ? yerr : yerr.substr( c + 2 )) );
		return false;
	}
	if( !root.IsMap() ){ err( 1, "マニフェストの最上位はマップにしてください" ); return false; }

	const std::string ver = root.Str( "manifest" );
	if( ver != "1" ) err( root.line, "manifest: 1 を指定してください（対応しているのはバージョン 1）" );
	if( project.empty() ) project = root.Str( "project" );

	// include（先に読み込む）
	if( const Node* inc = root.Get( "include" ) ){
		if( !inc->IsSeq() ) err( inc->line, "include はシーケンスにしてください" );
		else if( depth > 16 ) err( inc->line, "include が深すぎます（循環していませんか）" );
		else {
			for( const Node& n : inc->seq ){
				std::string path = baseDir + n.scalar;
				bool seen = false;
				for( const std::string& p : m_loaded ) if( p == path ) seen = true;
				if( seen ) continue;
				std::string sub;
				if( !ReadFile( path, &sub ) ){ err( n.line, "include '" + n.scalar + "' を開けません" ); continue; }
				m_loaded.push_back( path );
				LoadImpl( sub, path, DirOf( path ), diag, depth + 1 );
			}
		}
	}

	// enums（型の解決に使うので先に読む）
	if( const Node* en = root.Get( "enums" ) ){
		if( !en->IsMap() ) err( en->line, "enums はマップにしてください" );
		else for( const auto& kv : en->map ){
			if( FindEnum( kv.first ) ){ err( kv.second.line, "enum '" + kv.first + "' が重複しています" ); continue; }
			MEnum e;
			e.name    = kv.first;
			e.display = kv.second.Str( "display" );
			e.file    = file;
			e.line    = kv.second.line;
			const Node* values = kv.second.Get( "values" );
			if( !values || !values->IsMap() ){ err( kv.second.line, "enum '" + kv.first + "' に values がありません" ); continue; }
			for( const auto& v : values->map ){
				long long n;
				if( !ParseInt( v.second.scalar, &n ) ){ err( v.second.line, "enum の値は整数にしてください：" + v.first ); continue; }
				int32_t dummy;
				if( e.Find( v.first, &dummy ) ){ err( v.second.line, "enum の値 '" + v.first + "' が重複しています" ); continue; }
				e.values.emplace_back( v.first, (int32_t)n );
			}
			enums.push_back( e );
		}
	}

	auto readType = [&]( const Node& owner, const char* key, TypeRef* out, bool allowVoid ) -> bool {
		const Node* t = owner.Get( key );
		if( !t || t->IsNull() ){
			if( allowVoid ){ *out = TypeRef(); return true; }
			err( owner.line, std::string( key ) + " がありません" );
			return false;
		}
		if( !ResolveType( t->scalar, out ) ){ err( t->line, "未知の型 '" + t->scalar + "'" ); return false; }
		return true;
	};

	auto readParams = [&]( const Node& owner, std::vector<MParam>* out ) {
		const Node* ps = owner.Get( "params" );
		if( !ps || ps->IsNull() ) return;
		if( !ps->IsSeq() ){ err( ps->line, "params はシーケンスにしてください" ); return; }
		for( const Node& p : ps->seq ){
			MParam mp;
			mp.name    = p.Str( "name" );
			mp.display = p.Str( "display" );
			if( mp.name.empty() ){ err( p.line, "引数に name がありません" ); continue; }
			if( !readType( p, "type", &mp.type, false ) ) continue;
			mp.def = ToLiteral( p.Get( "default" ) );
			for( const MParam& o : *out ) if( o.name == mp.name ) err( p.line, "引数 '" + mp.name + "' が重複しています" );
			out->push_back( mp );
		}
		if( out->size() > 8 ) err( ps->line, "引数は 8 個までです" );
	};

	// variable_banks
	if( const Node* vb = root.Get( "variable_banks" ) ){
		if( !vb->IsMap() ) err( vb->line, "variable_banks はマップにしてください" );
		else for( const auto& bank : vb->map ){
			if( !IsBank( bank.first ) ) banks.push_back( bank.first );
			std::string scope = bank.second.Str( "scope", "persistent" );
			uint32_t sc;
			if( scope == "persistent" ) sc = ATS_SCOPE_PERSISTENT;
			else if( scope == "session" ) sc = ATS_SCOPE_SESSION;
			else { err( bank.second.line, "scope は persistent か session にしてください" ); continue; }
			const Node* vs = bank.second.Get( "vars" );
			if( !vs || !vs->IsSeq() ){ err( bank.second.line, "バンク '" + bank.first + "' に vars がありません" ); continue; }
			for( const Node& v : vs->seq ){
				MVar mv;
				mv.bank  = bank.first;
				mv.name  = v.Str( "name" );
				mv.scope = sc;
				mv.line  = v.line;
				mv.file  = file;
				long long id;
				if( mv.name.empty() ){ err( v.line, "変数に name がありません" ); continue; }
				if( !ParseInt( v.Str( "id" ), &id ) || id < 0 || id > 0xFFFFFFFFll ){ err( v.line, "変数 '" + mv.name + "' の id が不正です" ); continue; }
				mv.id = (uint32_t)id;
				if( !readType( v, "type", &mv.type, false ) ) continue;
				mv.init = ToLiteral( v.Get( "init" ) );
				if( FindVarById( mv.id ) ){ err( v.line, "変数 id " + std::to_string( mv.id ) + " が重複しています" ); continue; }
				if( FindVar( mv.bank, mv.name ) ){ err( v.line, "変数 '" + mv.bank + "." + mv.name + "' が重複しています" ); continue; }
				vars.push_back( mv );
			}
		}
	}

	// events
	if( const Node* ev = root.Get( "events" ) ){
		if( !ev->IsSeq() ) err( ev->line, "events はシーケンスにしてください" );
		else for( const Node& e : ev->seq ){
			MEvent me;
			me.name = e.Str( "name" );
			me.file = file;
			me.line = e.line;
			if( me.name.empty() ){ err( e.line, "イベントに name がありません" ); continue; }
			if( FindEvent( me.name ) ){ err( e.line, "イベント '" + me.name + "' が重複しています" ); continue; }
			readParams( e, &me.params );
			events.push_back( me );
		}
	}

	// commands / queries
	auto readCommands = [&]( const char* key, bool query ) {
		const Node* cs = root.Get( key );
		if( !cs ) return;
		if( !cs->IsSeq() ){ err( cs->line, std::string( key ) + " はシーケンスにしてください" ); return; }
		for( const Node& c : cs->seq ){
			MCommand mc;
			mc.name        = c.Str( "name" );
			mc.display     = c.Str( "display" );
			mc.category    = c.Str( "category" );
			mc.description = c.Str( "description" );
			mc.channel     = c.Str( "channel" );
			mc.query       = query;
			mc.line        = c.line;
			mc.file        = file;
			if( mc.name.empty() ){ err( c.line, "name がありません" ); continue; }
			bool b = false;
			if( c.Get( "latent" ) && !ParseBool( c.Str( "latent" ), &b ) ) err( c.line, "latent は true / false にしてください" );
			mc.latent = b;
			b = false;
			if( c.Get( "deprecated" ) ) ParseBool( c.Str( "deprecated" ), &b );
			mc.deprecated = b;
			if( !readType( c, "returns", &mc.ret, !query ) ) continue;
			if( query && mc.latent ) err( c.line, "クエリ '" + mc.name + "' に latent は指定できません" );
			if( query && !mc.channel.empty() ) err( c.line, "クエリ '" + mc.name + "' に channel は指定できません" );
			readParams( c, &mc.params );
			if( FindCommand( mc.name ) ){ err( c.line, "'" + mc.name + "' が重複しています（コマンドとクエリは同じ名前空間）" ); continue; }
			commands.push_back( mc );
		}
	};
	readCommands( "commands", false );
	readCommands( "queries", true );

	return diag.ErrorCount() == errorsBefore;
}

}	// namespace compiler
}	// namespace ats
