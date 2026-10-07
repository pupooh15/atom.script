/**************************************************************************/
/*!	\file	ats_compiler.cpp
	\brief	意味検査とコード生成
***************************************************************************/
#include "ats_compiler.h"
#include "ats_parser.h"
#include "ats_writer.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace ats {
namespace compiler {

using namespace ats::fmt;
using writer::Label;
using writer::ProgramWriter;

namespace {

//=========================================================================
// 型
//=========================================================================
TypeRef MakeType( ats_type t ) { TypeRef r; r.base = t; return r; }
const TypeRef kVoid  = MakeType( ATS_TYPE_VOID );
const TypeRef kBool  = MakeType( ATS_TYPE_BOOL );
const TypeRef kInt   = MakeType( ATS_TYPE_INT );
const TypeRef kFloat = MakeType( ATS_TYPE_FLOAT );
const TypeRef kStr   = MakeType( ATS_TYPE_STRING );
TypeRef MakeErrType() { TypeRef r; r.error = true; return r; }
const TypeRef kErr   = MakeErrType();	// エラー（連鎖して報告しないための印。ats_type の範囲外の値は使わない）

bool IsErr( const TypeRef& t )		{ return t.error; }
bool IsNumeric( const TypeRef& t )	{ return t.base == ATS_TYPE_INT || t.base == ATS_TYPE_FLOAT; }

// source を target に代入できるか（int → float は暗黙に変換する）
bool Assignable( const TypeRef& target, const TypeRef& source )
{
	if( target == source ) return true;
	if( target.base == ATS_TYPE_FLOAT && source.base == ATS_TYPE_INT ) return true;
	return false;
}

const char* BuiltinNames[] = { "int", "float", "rand", "min", "max", "abs" };
bool IsBuiltin( const std::string& s )
{
	for( const char* b : BuiltinNames ) if( s == b ) return true;
	return false;
}

//=========================================================================
// コンパイラ
//=========================================================================
struct ScriptVarInfo {
	uint16_t	index;
	TypeRef		type;
};

struct FnInfo {
	const FuncDecl*			decl = nullptr;
	uint16_t				entry = 0;
	std::vector<TypeRef>	params;
	TypeRef					ret;
	bool					latent = false;
	std::set<std::string>	callees;
};

struct Local {
	std::string	name;
	uint16_t	slot;
	TypeRef		type;
	int			branchDepth;
};

struct LoopCtx {
	Label	brk;
	Label	cont;
	int		branchDepth;
};

class Compiler {
public:
	Compiler( const Manifest& m, Diagnostics& d, const std::string& file, const CompileOptions& opt )
		: M( m ), D( d ), m_file( file ), m_opt( opt ), W( "" ) {}

	bool Run( const Script& s, CompileResult* out );

private:
	//---------------------------------------------------------------------
	// 報告
	//---------------------------------------------------------------------
	void Err( const Pos& p, const std::string& msg )  { D.Error( m_file, p.line, p.col, msg ); }
	void Warn( const Pos& p, const std::string& msg ) { D.Warning( m_file, p.line, p.col, msg ); }

	bool ResolveType( const std::string& name, const Pos& p, TypeRef* out )
	{
		if( M.ResolveType( name, out ) ) return true;
		Err( p, "未知の型 '" + name + "'" );
		*out = kErr;
		return false;
	}

	//---------------------------------------------------------------------
	// テーブル
	//---------------------------------------------------------------------
	uint16_t	ImportOf( const MCommand& c );
	uint16_t	SharedVarOf( const MVar& v );

	//---------------------------------------------------------------------
	// 定数
	//---------------------------------------------------------------------
	bool ConstExpr( const Expr& e, const TypeRef& type, uint64_t* slot, std::string* str );
	bool ConstLiteral( const Literal& lit, const TypeRef& type, uint64_t* slot, std::string* str, const Pos& p );
	void EmitConst( const TypeRef& type, uint64_t slot, const std::string& str );

	//---------------------------------------------------------------------
	// 宣言
	//---------------------------------------------------------------------
	void DeclareVars( const Script& s );
	void DeclareFuncs( const Script& s );
	void AnalyzeLatency();
	void ScanLatency( const Block& b, FnInfo& f, bool& direct );
	void ScanLatencyStmt( const Stmt& s, FnInfo& f, bool& direct );
	void ScanLatencyExpr( const Expr& e, FnInfo& f, bool& direct );
	void GenFunc( const FuncDecl& fd );

	//---------------------------------------------------------------------
	// 文
	//---------------------------------------------------------------------
	void		GenBlock( const Block& b );
	void		GenStmt( const Stmt& s );
	void		GenAssign( const Stmt& s );
	void		GenSwitch( const Stmt& s );
	void		GenLoop( const Stmt& s );
	void		GenParallel( const Stmt& s );
	void		GenFire( const Stmt& s );
	bool		AllPathsReturn( const Block& b ) const;
	bool		StmtReturns( const Stmt& s ) const;
	bool		LoopHasBreak( const Block& b, int depth ) const;
	bool		BlockWaits( const Block& b ) const;
	bool		StmtWaits( const Stmt& s ) const;
	bool		ExprWaits( const Expr& e ) const;

	//---------------------------------------------------------------------
	// 式
	//---------------------------------------------------------------------
	TypeRef		TypeOf( const Expr& e );
	TypeRef		GenExpr( const Expr& e );
	bool		GenExprAs( const Expr& e, const TypeRef& target, const char* what );
	TypeRef		GenCall( const Expr& e, bool asStatement );
	TypeRef		GenBuiltin( const Expr& e );
	TypeRef		GenBinary( const Expr& e );
	bool		BindArgs( const Expr& call, const std::vector<std::string>& names, const std::vector<TypeRef>& types,
						  const std::vector<const Literal*>& defaults, const std::string& what );
	TypeRef		BinaryType( Tok op, const TypeRef& a, const TypeRef& b ) const;

	//---------------------------------------------------------------------
	// スコープ
	//---------------------------------------------------------------------
	const Local*	FindLocal( const std::string& name ) const;
	bool			DeclareLocal( const std::string& name, const TypeRef& type, const Pos& p, uint16_t* slot );
	uint16_t		AllocTemp();
	void			PushScope() { m_scopes.emplace_back(); }
	void			PopScope()  { m_scopes.pop_back(); }

	const Manifest&							M;
	Diagnostics&							D;
	std::string								m_file;
	CompileOptions							m_opt;
	ProgramWriter							W;

	std::map<std::string, ScriptVarInfo>	m_vars;
	std::map<std::string, FnInfo>			m_fns;		// 関数（イベントを含まない）
	std::map<std::string, const FuncDecl*>	m_events;
	std::map<std::string, uint16_t>			m_imports;
	std::map<uint32_t, uint16_t>			m_shared;

	// 関数ごとの状態
	const FuncDecl*							m_cur = nullptr;
	TypeRef									m_curRet;
	std::vector<std::vector<Local>>			m_scopes;
	uint16_t								m_nextSlot = 0;
	uint16_t								m_maxSlot = 0;
	int										m_branchDepth = 0;
	std::vector<LoopCtx>					m_loops;
};

//=========================================================================
// テーブル
//=========================================================================
uint16_t Compiler::ImportOf( const MCommand& c )
{
	auto it = m_imports.find( c.name );
	if( it != m_imports.end() ) return it->second;
	std::vector<ats_type> params;
	for( const MParam& p : c.params ) params.push_back( p.type.base );
	uint16_t idx = c.query ? W.Query( c.name, params, c.ret.base, c.SignatureHash() )
						   : W.Command( c.name, params, c.ret.base, c.SignatureHash() );
	m_imports[c.name] = idx;
	return idx;
}

uint16_t Compiler::SharedVarOf( const MVar& v )
{
	auto it = m_shared.find( v.id );
	if( it != m_shared.end() ) return it->second;
	uint16_t idx = W.SharedVar( v.id, v.type.base );
	m_shared[v.id] = idx;
	return idx;
}

//=========================================================================
// 定数
//=========================================================================
bool Compiler::ConstExpr( const Expr& e, const TypeRef& type, uint64_t* slot, std::string* str )
{
	auto bad = [&]() { Err( e.pos, "定数（リテラルか enum の値）が必要です" ); return false; };
	auto mismatch = [&]( const std::string& have ) {
		Err( e.pos, "型が合いません：" + type.Name() + " が必要ですが " + have + " です" );
		return false;
	};
	switch( e.kind ){
	case Expr::IntLit:
		if( type.base == ATS_TYPE_INT ){ *slot = (uint32_t)(int32_t)e.ival; return true; }
		if( type.base == ATS_TYPE_FLOAT ){ float f = (float)e.ival; uint32_t u; std::memcpy( &u, &f, 4 ); *slot = u; return true; }
		return mismatch( "int" );
	case Expr::FloatLit:
		if( type.base == ATS_TYPE_FLOAT ){ float f = (float)e.fval; uint32_t u; std::memcpy( &u, &f, 4 ); *slot = u; return true; }
		return mismatch( "float" );
	case Expr::BoolLit:
		if( type.base == ATS_TYPE_BOOL ){ *slot = (uint64_t)e.ival; return true; }
		return mismatch( "bool" );
	case Expr::StrLit:
		if( type.base == ATS_TYPE_STRING ){ *str = e.text; return true; }
		return mismatch( "string" );
	case Expr::Unary:
		if( e.op == Tok::Minus && e.a && (e.a->kind == Expr::IntLit || e.a->kind == Expr::FloatLit) ){
			if( type.base == ATS_TYPE_INT && e.a->kind == Expr::IntLit ){ *slot = (uint32_t)(int32_t)(-e.a->ival); return true; }
			if( type.base == ATS_TYPE_FLOAT ){
				float f = (float)-(e.a->kind == Expr::IntLit ? (double)e.a->ival : e.a->fval);
				uint32_t u; std::memcpy( &u, &f, 4 ); *slot = u; return true;
			}
			return mismatch( e.a->kind == Expr::IntLit ? "int" : "float" );
		}
		return bad();
	case Expr::Member: {
		const MEnum* en = M.FindEnum( e.owner );
		if( !en ) return bad();
		int32_t v;
		if( !en->Find( e.text, &v ) ){ Err( e.pos, "enum '" + e.owner + "' に値 '" + e.text + "' はありません" ); return false; }
		if( type.base != ATS_TYPE_ENUM || type.enumName != e.owner ) return mismatch( e.owner );
		*slot = (uint32_t)v;
		return true;
	}
	default:
		return bad();
	}
}

bool Compiler::ConstLiteral( const Literal& lit, const TypeRef& type, uint64_t* slot, std::string* str, const Pos& p )
{
	auto bad = [&]() {
		Err( p, "マニフェストの既定値 '" + lit.text + "' は " + type.Name() + " として読めません" );
		return false;
	};
	const std::string& t = lit.text;
	char* end = nullptr;
	switch( type.base ){
	case ATS_TYPE_BOOL:
		if( t == "true" ){ *slot = 1; return true; }
		if( t == "false" ){ *slot = 0; return true; }
		return bad();
	case ATS_TYPE_INT: {
		long long v = std::strtoll( t.c_str(), &end, 0 );
		if( t.empty() || *end ) return bad();
		*slot = (uint32_t)(int32_t)v;
		return true;
	}
	case ATS_TYPE_FLOAT: {
		double v = std::strtod( t.c_str(), &end );
		if( t.empty() || *end ) return bad();
		float f = (float)v; uint32_t u; std::memcpy( &u, &f, 4 ); *slot = u;
		return true;
	}
	case ATS_TYPE_STRING:
		*str = t;
		return true;
	case ATS_TYPE_ENUM: {
		const MEnum* en = M.FindEnum( type.enumName );
		int32_t v;
		std::string name = t;
		size_t dot = name.find( '.' );
		if( dot != std::string::npos ) name = name.substr( dot + 1 );	// "Face.Smile" も可
		if( !en || !en->Find( name, &v ) ) return bad();
		*slot = (uint32_t)v;
		return true;
	}
	case ATS_TYPE_HANDLE:
		if( t == "0" || t == "null" ){ *slot = 0; return true; }
		return bad();
	default:
		return bad();
	}
}

void Compiler::EmitConst( const TypeRef& type, uint64_t slot, const std::string& str )
{
	switch( type.base ){
	case ATS_TYPE_STRING:	W.PushStr( str ); break;
	case ATS_TYPE_FLOAT:	{ float f; uint32_t u = (uint32_t)slot; std::memcpy( &f, &u, 4 ); W.PushF( f ); break; }
	case ATS_TYPE_HANDLE:	W.PushK( slot ); break;
	default:				W.PushI( (int32_t)(uint32_t)slot ); break;
	}
}

//=========================================================================
// 宣言
//=========================================================================
void Compiler::DeclareVars( const Script& s )
{
	for( const VarDecl& v : s.vars ){
		TypeRef type;
		if( !ResolveType( v.typeName, v.typePos, &type ) ) continue;
		if( m_vars.count( v.name ) ){ Err( v.pos, "var '" + v.name + "' が重複しています" ); continue; }
		if( M.IsBank( v.name ) || M.FindEnum( v.name ) ){
			Err( v.pos, "'" + v.name + "' はマニフェストのバンク名・enum 名と同じなので使えません" );
			continue;
		}
		uint64_t slot = 0;
		std::string str;
		if( !ConstExpr( *v.init, type, &slot, &str ) ) continue;
		if( type.base == ATS_TYPE_HANDLE ){ Err( v.pos, "handle 型の var は作れません（セーブできないため）" ); continue; }
		ScriptVarInfo info;
		info.type  = type;
		info.index = type.base == ATS_TYPE_STRING ? W.ScriptVarStr( v.name, str, v.transient, v.was )
												  : W.ScriptVar( v.name, type.base, slot, v.transient, v.was );
		m_vars[v.name] = info;
	}
}

void Compiler::DeclareFuncs( const Script& s )
{
	for( const FuncDecl& f : s.funcs ){
		if( f.params.size() > (size_t)kMaxParams ) Err( f.pos, "引数は 8 個までです" );
		if( f.isEvent ){
			if( m_events.count( f.name ) ){ Err( f.pos, "イベント '" + f.name + "' が重複しています" ); continue; }
			m_events[f.name] = &f;
			// マニフェストに宣言されたイベントなら引数を照合する
			if( const MEvent* me = M.FindEvent( f.name ) ){
				bool ok = me->params.size() == f.params.size();
				for( size_t i = 0; ok && i < f.params.size(); ++i ){
					TypeRef t;
					ok = M.ResolveType( f.params[i].typeName, &t ) && t == me->params[i].type;
				}
				if( !ok ){
					std::string sig;
					for( size_t i = 0; i < me->params.size(); ++i ) sig += (i ? ", " : "") + me->params[i].name + ": " + me->params[i].type.Name();
					Err( f.pos, "イベント '" + f.name + "' の引数がマニフェストと違います（(" + sig + ") にしてください）" );
				}
			}
			continue;
		}
		if( m_fns.count( f.name ) ){ Err( f.pos, "関数 '" + f.name + "' が重複しています" ); continue; }
		if( IsBuiltin( f.name ) ){ Err( f.pos, "'" + f.name + "' は組み込み関数と同じ名前なので使えません" ); continue; }
		if( M.FindCommand( f.name ) ){ Err( f.pos, "'" + f.name + "' はマニフェストのコマンド・クエリと同じ名前なので使えません" ); continue; }
		FnInfo fi;
		fi.decl = &f;
		std::vector<ats_type> ptypes;
		for( const Param& p : f.params ){
			TypeRef t;
			ResolveType( p.typeName, p.pos, &t );
			fi.params.push_back( t );
			ptypes.push_back( t.base );
		}
		fi.ret = kVoid;
		if( !f.retType.empty() ) ResolveType( f.retType, f.retPos, &fi.ret );
		fi.entry = W.DeclareFunction( f.name, ptypes, IsErr( fi.ret ) ? ATS_TYPE_VOID : fi.ret.base, (uint16_t)f.params.size() );
		m_fns[f.name] = fi;
	}
}

// 待機を含む関数（呼び出しに await が必要）を求める
void Compiler::ScanLatencyExpr( const Expr& e, FnInfo& f, bool& direct )
{
	if( e.kind == Expr::Call ){
		if( m_fns.count( e.text ) ) f.callees.insert( e.text );
		else if( const MCommand* c = M.FindCommand( e.text ) ){ if( c->latent ) direct = true; }
		for( const Arg& a : e.args ) if( a.value ) ScanLatencyExpr( *a.value, f, direct );
	}
	if( e.a ) ScanLatencyExpr( *e.a, f, direct );
	if( e.b ) ScanLatencyExpr( *e.b, f, direct );
}

void Compiler::ScanLatencyStmt( const Stmt& s, FnInfo& f, bool& direct )
{
	switch( s.kind ){
	case Stmt::Wait: case Stmt::WaitFrames: case Stmt::Yield: case Stmt::Parallel:
		direct = true;
		break;
	default: break;
	}
	if( s.value ) ScanLatencyExpr( *s.value, f, direct );
	if( s.target ) ScanLatencyExpr( *s.target, f, direct );
	for( const Arg& a : s.args ) if( a.value ) ScanLatencyExpr( *a.value, f, direct );
	ScanLatency( s.body, f, direct );
	if( s.elseStmt ) ScanLatencyStmt( *s.elseStmt, f, direct );
	for( const Case& c : s.cases ) ScanLatency( c.body, f, direct );
	for( const Block& br : s.branches ) ScanLatency( br, f, direct );
}

void Compiler::ScanLatency( const Block& b, FnInfo& f, bool& direct )
{
	for( const StmtPtr& sp : b.stmts ) ScanLatencyStmt( *sp, f, direct );
}

void Compiler::AnalyzeLatency()
{
	for( auto& kv : m_fns ){
		bool direct = false;
		ScanLatency( kv.second.decl->body, kv.second, direct );
		kv.second.latent = direct;
	}
	// 呼び出し先が待機するなら呼び出し元も待機する（不動点まで繰り返す）
	for( bool changed = true; changed; ){
		changed = false;
		for( auto& kv : m_fns ){
			if( kv.second.latent ) continue;
			for( const std::string& c : kv.second.callees ){
				if( m_fns[c].latent ){ kv.second.latent = true; changed = true; break; }
			}
		}
	}
}

//=========================================================================
// スコープ
//=========================================================================
const Local* Compiler::FindLocal( const std::string& name ) const
{
	for( auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it )
		for( const Local& l : *it ) if( l.name == name ) return &l;
	return nullptr;
}

bool Compiler::DeclareLocal( const std::string& name, const TypeRef& type, const Pos& p, uint16_t* slot )
{
	if( FindLocal( name ) ){ Err( p, "'" + name + "' は既に宣言されています" ); return false; }
	if( m_vars.count( name ) ){ Err( p, "'" + name + "' は var と同じ名前なので使えません" ); return false; }
	if( M.IsBank( name ) || M.FindEnum( name ) ){ Err( p, "'" + name + "' はマニフェストのバンク名・enum 名と同じなので使えません" ); return false; }
	Local l;
	l.name        = name;
	l.type        = type;
	l.slot        = m_nextSlot++;
	l.branchDepth = m_branchDepth;
	if( m_nextSlot > m_maxSlot ) m_maxSlot = m_nextSlot;
	m_scopes.back().push_back( l );
	*slot = l.slot;
	return true;
}

uint16_t Compiler::AllocTemp()
{
	uint16_t s = m_nextSlot++;
	if( m_nextSlot > m_maxSlot ) m_maxSlot = m_nextSlot;
	return s;
}

//=========================================================================
// 式の型（コードを出さずに求める。数値の昇格の判断に使う）
//=========================================================================
TypeRef Compiler::BinaryType( Tok op, const TypeRef& a, const TypeRef& b ) const
{
	if( IsErr( a ) || IsErr( b ) ) return kErr;
	switch( op ){
	case Tok::AndAnd: case Tok::OrOr:
		return (a.base == ATS_TYPE_BOOL && b.base == ATS_TYPE_BOOL) ? kBool : kErr;
	case Tok::Eq: case Tok::Ne:
		if( IsNumeric( a ) && IsNumeric( b ) ) return kBool;
		return a == b ? kBool : kErr;
	case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge:
		return (IsNumeric( a ) && IsNumeric( b )) ? kBool : kErr;
	case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash:
		if( !IsNumeric( a ) || !IsNumeric( b ) ) return kErr;
		return (a.base == ATS_TYPE_FLOAT || b.base == ATS_TYPE_FLOAT) ? kFloat : kInt;
	case Tok::Percent: case Tok::Amp: case Tok::Pipe: case Tok::Caret: case Tok::Shl: case Tok::Shr:
		return (a.base == ATS_TYPE_INT && b.base == ATS_TYPE_INT) ? kInt : kErr;
	default:
		return kErr;
	}
}

TypeRef Compiler::TypeOf( const Expr& e )
{
	switch( e.kind ){
	case Expr::IntLit:		return kInt;
	case Expr::FloatLit:	return kFloat;
	case Expr::StrLit:		return kStr;
	case Expr::BoolLit:		return kBool;
	case Expr::Name: {
		if( const Local* l = FindLocal( e.text ) ) return l->type;
		auto it = m_vars.find( e.text );
		return it != m_vars.end() ? it->second.type : kErr;
	}
	case Expr::Member: {
		if( M.FindEnum( e.owner ) ){ TypeRef t; t.base = ATS_TYPE_ENUM; t.enumName = e.owner; return t; }
		if( const MVar* v = M.FindVar( e.owner, e.text ) ) return v->type;
		return kErr;
	}
	case Expr::Unary: {
		TypeRef t = TypeOf( *e.a );
		if( e.op == Tok::Bang ) return t.base == ATS_TYPE_BOOL ? kBool : kErr;
		if( e.op == Tok::Tilde ) return t.base == ATS_TYPE_INT ? kInt : kErr;
		return IsNumeric( t ) ? t : kErr;
	}
	case Expr::Binary:
		return BinaryType( e.op, TypeOf( *e.a ), TypeOf( *e.b ) );
	case Expr::Call: {
		const std::string& n = e.text;
		if( n == "int" || n == "rand" ) return kInt;
		if( n == "float" ) return kFloat;
		if( n == "min" || n == "max" ){
			if( e.args.size() != 2 ) return kErr;
			TypeRef a = TypeOf( *e.args[0].value ), b = TypeOf( *e.args[1].value );
			if( !IsNumeric( a ) || !IsNumeric( b ) ) return kErr;
			return (a.base == ATS_TYPE_FLOAT || b.base == ATS_TYPE_FLOAT) ? kFloat : kInt;
		}
		if( n == "abs" ) return e.args.size() == 1 ? TypeOf( *e.args[0].value ) : kErr;
		auto it = m_fns.find( n );
		if( it != m_fns.end() ) return it->second.ret;
		if( const MCommand* c = M.FindCommand( n ) ) return c->ret;
		return kErr;
	}
	}
	return kErr;
}

//=========================================================================
// 式の生成
//=========================================================================
bool Compiler::GenExprAs( const Expr& e, const TypeRef& target, const char* what )
{
	TypeRef t = GenExpr( e );
	if( IsErr( t ) || IsErr( target ) ) return false;
	if( t == target ) return true;
	if( target.base == ATS_TYPE_FLOAT && t.base == ATS_TYPE_INT ){ W.Op( OP_I2F ); return true; }
	if( t.IsVoid() ){ Err( e.pos, std::string( what ) + "：値を返さない呼び出しは使えません" ); return false; }
	Err( e.pos, std::string( what ) + "：型が合いません（" + target.Name() + " が必要ですが " + t.Name() + " です）" );
	return false;
}

TypeRef Compiler::GenExpr( const Expr& e )
{
	switch( e.kind ){
	case Expr::IntLit:
		W.PushI( (int32_t)(uint32_t)e.ival );
		return kInt;
	case Expr::FloatLit:
		W.PushF( (float)e.fval );
		return kFloat;
	case Expr::StrLit:
		W.PushStr( e.text );
		return kStr;
	case Expr::BoolLit:
		W.PushI( (int32_t)e.ival );
		return kBool;

	case Expr::Name: {
		if( const Local* l = FindLocal( e.text ) ){ W.LdLocal( l->slot ); return l->type; }
		auto it = m_vars.find( e.text );
		if( it != m_vars.end() ){ W.LdVar( it->second.index ); return it->second.type; }
		if( M.IsBank( e.text ) ) Err( e.pos, "'" + e.text + "' はバンク名です（" + e.text + ".<変数名> と書いてください）" );
		else if( M.FindEnum( e.text ) ) Err( e.pos, "'" + e.text + "' は enum 名です（" + e.text + ".<値> と書いてください）" );
		else if( m_fns.count( e.text ) || M.FindCommand( e.text ) ) Err( e.pos, "'" + e.text + "' を呼び出すには () が必要です" );
		else Err( e.pos, "'" + e.text + "' は宣言されていません" );
		return kErr;
	}

	case Expr::Member: {
		if( const MEnum* en = M.FindEnum( e.owner ) ){
			int32_t v;
			if( !en->Find( e.text, &v ) ){ Err( e.pos, "enum '" + e.owner + "' に値 '" + e.text + "' はありません" ); return kErr; }
			W.PushI( v );
			TypeRef t;
			t.base     = ATS_TYPE_ENUM;
			t.enumName = e.owner;
			return t;
		}
		if( M.IsBank( e.owner ) ){
			const MVar* v = M.FindVar( e.owner, e.text );
			if( !v ){ Err( e.pos, "バンク '" + e.owner + "' に変数 '" + e.text + "' はありません" ); return kErr; }
			W.LdVar( SharedVarOf( *v ) );
			return v->type;
		}
		Err( e.pos, "'" + e.owner + "' はバンク名でも enum 名でもありません" );
		return kErr;
	}

	case Expr::Unary: {
		TypeRef ta = TypeOf( *e.a );
		if( e.op == Tok::Bang ){
			if( !GenExprAs( *e.a, kBool, "'!' の対象" ) ) return kErr;
			W.Op( OP_NOT );
			return kBool;
		}
		if( e.op == Tok::Tilde ){
			if( !GenExprAs( *e.a, kInt, "'~' の対象" ) ) return kErr;
			W.Op( OP_BNOT );
			return kInt;
		}
		if( !IsNumeric( ta ) ){
			TypeRef t = GenExpr( *e.a );
			if( !IsErr( t ) ) Err( e.pos, "'-' は int か float にしか使えません" );
			return kErr;
		}
		GenExpr( *e.a );
		W.Op( ta.base == ATS_TYPE_FLOAT ? OP_NEG_F : OP_NEG_I );
		return ta;
	}

	case Expr::Binary:
		return GenBinary( e );

	case Expr::Call:
		return GenCall( e, false );
	}
	return kErr;
}

TypeRef Compiler::GenBinary( const Expr& e )
{
	// 短絡評価
	if( e.op == Tok::AndAnd || e.op == Tok::OrOr ){
		Label end = W.NewLabel();
		if( !GenExprAs( *e.a, kBool, e.op == Tok::AndAnd ? "'&&' の左辺" : "'||' の左辺" ) ) return kErr;
		W.Op( OP_DUP );
		if( e.op == Tok::AndAnd ) W.Jz( end ); else W.Jnz( end );
		W.Op( OP_POP );
		if( !GenExprAs( *e.b, kBool, e.op == Tok::AndAnd ? "'&&' の右辺" : "'||' の右辺" ) ) return kErr;
		W.Bind( end );
		return kBool;
	}

	TypeRef ta = TypeOf( *e.a ), tb = TypeOf( *e.b );
	TypeRef rt = BinaryType( e.op, ta, tb );
	if( IsErr( rt ) ){
		// 片方が未宣言などのエラーなら、その報告に任せる
		TypeRef ga = GenExpr( *e.a ), gb = GenExpr( *e.b );
		if( !IsErr( ga ) && !IsErr( gb ) )
			Err( e.pos, "この演算は " + ga.Name() + " と " + gb.Name() + " には使えません" );
		return kErr;
	}

	const bool isFloat = IsNumeric( ta ) && IsNumeric( tb ) && (ta.base == ATS_TYPE_FLOAT || tb.base == ATS_TYPE_FLOAT);
	const TypeRef operand = isFloat ? kFloat : ta;
	if( !GenExprAs( *e.a, operand, "左辺" ) || !GenExprAs( *e.b, isFloat ? kFloat : tb, "右辺" ) ) return kErr;

	Op op = OP_NOP;
	switch( e.op ){
	case Tok::Plus:		op = isFloat ? OP_ADD_F : OP_ADD_I; break;
	case Tok::Minus:	op = isFloat ? OP_SUB_F : OP_SUB_I; break;
	case Tok::Star:		op = isFloat ? OP_MUL_F : OP_MUL_I; break;
	case Tok::Slash:	op = isFloat ? OP_DIV_F : OP_DIV_I; break;
	case Tok::Percent:	op = OP_MOD_I; break;
	case Tok::Amp:		op = OP_BAND; break;
	case Tok::Pipe:		op = OP_BOR; break;
	case Tok::Caret:	op = OP_BXOR; break;
	case Tok::Shl:		op = OP_SHL; break;
	case Tok::Shr:		op = OP_SHR; break;
	case Tok::Lt:		op = isFloat ? OP_LT_F : OP_LT_I; break;
	case Tok::Le:		op = isFloat ? OP_LE_F : OP_LE_I; break;
	case Tok::Gt:		op = isFloat ? OP_GT_F : OP_GT_I; break;
	case Tok::Ge:		op = isFloat ? OP_GE_F : OP_GE_I; break;
	case Tok::Eq:
	case Tok::Ne: {
		bool eq = e.op == Tok::Eq;
		if( isFloat )							op = eq ? OP_EQ_F : OP_NE_F;
		else if( ta.base == ATS_TYPE_HANDLE )	op = eq ? OP_EQ_H : OP_NE_H;
		else									op = eq ? OP_EQ_I : OP_NE_I;	// int・bool・enum・string（文字列 ID）
		break;
	}
	default:
		Err( e.pos, "未対応の演算子です" );
		return kErr;
	}
	W.Op( op );
	return rt;
}

bool Compiler::BindArgs( const Expr& call, const std::vector<std::string>& names, const std::vector<TypeRef>& types,
						 const std::vector<const Literal*>& defaults, const std::string& what )
{
	// 引数を仮引数の並びに割り当てる（位置指定 → 名前指定）
	std::vector<const Arg*> bound( types.size(), nullptr );
	bool ok = true;
	bool namedSeen = false;
	size_t pos = 0;
	for( const Arg& a : call.args ){
		if( a.name.empty() ){
			if( namedSeen ){ Err( a.pos, "名前指定の引数の後ろに位置指定の引数は書けません" ); ok = false; continue; }
			if( pos >= types.size() ){ Err( a.pos, what + " の引数が多すぎます（" + std::to_string( types.size() ) + " 個）" ); ok = false; continue; }
			bound[pos++] = &a;
		} else {
			namedSeen = true;
			size_t k = 0;
			while( k < names.size() && names[k] != a.name ) ++k;
			if( k == names.size() ){ Err( a.pos, what + " に引数 '" + a.name + "' はありません" ); ok = false; continue; }
			if( bound[k] ){ Err( a.pos, "引数 '" + a.name + "' が 2 回指定されています" ); ok = false; continue; }
			bound[k] = &a;
		}
	}
	if( !ok ) return false;

	for( size_t i = 0; i < types.size(); ++i ){
		if( bound[i] ){
			std::string label = "引数 '" + names[i] + "'";
			if( !GenExprAs( *bound[i]->value, types[i], label.c_str() ) ) ok = false;
			continue;
		}
		const Literal* def = i < defaults.size() ? defaults[i] : nullptr;
		if( !def || !def->present ){
			Err( call.pos, what + " の引数 '" + names[i] + "' がありません" );
			ok = false;
			continue;
		}
		uint64_t slot = 0;
		std::string str;
		if( !ConstLiteral( *def, types[i], &slot, &str, call.pos ) ){ ok = false; continue; }
		EmitConst( types[i], slot, str );
	}
	return ok;
}

TypeRef Compiler::GenBuiltin( const Expr& e )
{
	const std::string& n = e.text;
	if( e.await ){ Err( e.pos, "'" + n + "' は待機しないので await は不要です" ); return kErr; }
	for( const Arg& a : e.args )
		if( !a.name.empty() ){ Err( a.pos, "組み込み関数に名前指定の引数は使えません" ); return kErr; }
	auto argc = [&]( size_t want ) {
		if( e.args.size() == want ) return true;
		Err( e.pos, "'" + n + "' の引数は " + std::to_string( want ) + " 個です" );
		return false;
	};

	if( n == "int" ){
		if( !argc( 1 ) ) return kErr;
		TypeRef t = GenExpr( *e.args[0].value );
		if( IsErr( t ) ) return kErr;
		if( t.base == ATS_TYPE_FLOAT ) W.Op( OP_F2I );
		else if( t.base != ATS_TYPE_INT && t.base != ATS_TYPE_ENUM && t.base != ATS_TYPE_BOOL ){
			Err( e.pos, "int() に " + t.Name() + " は渡せません" );
			return kErr;
		}
		return kInt;
	}
	if( n == "float" ){
		if( !argc( 1 ) ) return kErr;
		TypeRef t = GenExpr( *e.args[0].value );
		if( IsErr( t ) ) return kErr;
		if( t.base == ATS_TYPE_INT || t.base == ATS_TYPE_ENUM ) W.Op( OP_I2F );
		else if( t.base != ATS_TYPE_FLOAT ){ Err( e.pos, "float() に " + t.Name() + " は渡せません" ); return kErr; }
		return kFloat;
	}
	if( n == "rand" ){
		if( !argc( 1 ) || !GenExprAs( *e.args[0].value, kInt, "rand() の引数" ) ) return kErr;
		W.Op( OP_RAND );
		return kInt;
	}
	if( n == "abs" ){
		if( !argc( 1 ) ) return kErr;
		TypeRef t = TypeOf( *e.args[0].value );
		if( !IsNumeric( t ) ){ GenExpr( *e.args[0].value ); if( !IsErr( t ) ) Err( e.pos, "abs() は int か float に使います" ); return kErr; }
		uint16_t tmp = AllocTemp();
		GenExpr( *e.args[0].value );
		W.StLocal( tmp );
		Label neg = W.NewLabel(), end = W.NewLabel();
		W.LdLocal( tmp );
		if( t.base == ATS_TYPE_FLOAT ){ W.PushF( 0.0f ); W.Op( OP_LT_F ); } else { W.PushI( 0 ); W.Op( OP_LT_I ); }
		W.Jnz( neg );
		W.LdLocal( tmp );
		W.Jmp( end );
		W.Bind( neg );
		W.LdLocal( tmp );
		W.Op( t.base == ATS_TYPE_FLOAT ? OP_NEG_F : OP_NEG_I );
		W.Bind( end );
		return t;
	}
	// min / max
	if( !argc( 2 ) ) return kErr;
	TypeRef ta = TypeOf( *e.args[0].value ), tb = TypeOf( *e.args[1].value );
	if( !IsNumeric( ta ) || !IsNumeric( tb ) ){
		GenExpr( *e.args[0].value );
		GenExpr( *e.args[1].value );
		if( !IsErr( ta ) && !IsErr( tb ) ) Err( e.pos, n + "() は int か float に使います" );
		return kErr;
	}
	TypeRef t = (ta.base == ATS_TYPE_FLOAT || tb.base == ATS_TYPE_FLOAT) ? kFloat : kInt;
	uint16_t a = AllocTemp(), b = AllocTemp();
	GenExprAs( *e.args[0].value, t, "引数" ); W.StLocal( a );
	GenExprAs( *e.args[1].value, t, "引数" ); W.StLocal( b );
	Label useB = W.NewLabel(), end = W.NewLabel();
	W.LdLocal( a );
	W.LdLocal( b );
	if( n == "min" ) W.Op( t.base == ATS_TYPE_FLOAT ? OP_LE_F : OP_LE_I );
	else             W.Op( t.base == ATS_TYPE_FLOAT ? OP_GE_F : OP_GE_I );
	W.Jz( useB );
	W.LdLocal( a );
	W.Jmp( end );
	W.Bind( useB );
	W.LdLocal( b );
	W.Bind( end );
	return t;
}

TypeRef Compiler::GenCall( const Expr& e, bool asStatement )
{
	const std::string& n = e.text;
	TypeRef ret;

	if( IsBuiltin( n ) && !m_fns.count( n ) ){
		ret = GenBuiltin( e );
	} else if( m_fns.count( n ) ){
		const FnInfo& fi = m_fns[n];
		if( fi.latent && !e.await ) Err( e.pos, "関数 '" + n + "' は待機を含むので await が必要です" );
		if( !fi.latent && e.await ) Err( e.pos, "関数 '" + n + "' は待機しないので await は不要です" );
		std::vector<std::string> names;
		std::vector<const Literal*> defs;
		for( const Param& p : fi.decl->params ){ names.push_back( p.name ); defs.push_back( nullptr ); }
		if( !BindArgs( e, names, fi.params, defs, "関数 '" + n + "'" ) ) return kErr;
		W.CallFn( fi.entry );
		ret = fi.ret;
	} else if( const MCommand* c = M.FindCommand( n ) ){
		if( c->query && e.await ) Err( e.pos, "クエリ '" + n + "' は待機しないので await は不要です" );
		if( !c->query && c->latent && !e.await ) Err( e.pos, "'" + n + "' は待機ありのコマンドなので await が必要です" );
		if( !c->query && !c->latent && e.await ) Err( e.pos, "'" + n + "' は即時コマンドなので await は不要です" );
		if( c->deprecated ) Warn( e.pos, "'" + n + "' は非推奨です" );
		std::vector<std::string> names;
		std::vector<TypeRef> types;
		std::vector<const Literal*> defs;
		for( const MParam& p : c->params ){ names.push_back( p.name ); types.push_back( p.type ); defs.push_back( &p.def ); }
		if( !BindArgs( e, names, types, defs, "'" + n + "'" ) ) return kErr;
		uint16_t imp = ImportOf( *c );
		if( c->query ) W.CallQuery( imp ); else W.CallCmd( imp );
		ret = c->ret;
	} else {
		if( m_events.count( n ) ) Err( e.pos, "'" + n + "' はイベントなので直接呼べません（fire " + n + "(…) を使ってください）" );
		else Err( e.pos, "'" + n + "' は関数・コマンド・クエリのどれでもありません" );
		for( const Arg& a : e.args ) GenExpr( *a.value );
		return kErr;
	}

	if( IsErr( ret ) ) return kErr;
	if( asStatement && !ret.IsVoid() ) W.Op( OP_POP );
	return ret;
}

//=========================================================================
// 文
//=========================================================================
bool Compiler::ExprWaits( const Expr& e ) const
{
	if( e.kind == Expr::Call && e.await ) return true;
	if( e.a && ExprWaits( *e.a ) ) return true;
	if( e.b && ExprWaits( *e.b ) ) return true;
	for( const Arg& a : e.args ) if( a.value && ExprWaits( *a.value ) ) return true;
	return false;
}

bool Compiler::StmtWaits( const Stmt& s ) const
{
	switch( s.kind ){
	case Stmt::Wait: case Stmt::WaitFrames: case Stmt::Yield: case Stmt::Parallel: return true;
	default: break;
	}
	if( s.value && ExprWaits( *s.value ) ) return true;
	if( BlockWaits( s.body ) ) return true;
	if( s.elseStmt && StmtWaits( *s.elseStmt ) ) return true;
	for( const Case& c : s.cases ) if( BlockWaits( c.body ) ) return true;
	return false;
}

bool Compiler::BlockWaits( const Block& b ) const
{
	for( const StmtPtr& s : b.stmts ) if( StmtWaits( *s ) ) return true;
	return false;
}

// このループ（depth = 0）から抜ける break があるか
bool Compiler::LoopHasBreak( const Block& b, int depth ) const
{
	for( const StmtPtr& sp : b.stmts ){
		const Stmt& s = *sp;
		if( s.kind == Stmt::Break && depth == 0 ) return true;
		if( s.kind == Stmt::Return ) return true;
		int d = s.kind == Stmt::Loop ? depth + 1 : depth;
		if( LoopHasBreak( s.body, d ) ) return true;
		if( s.elseStmt ){
			const Stmt* e = s.elseStmt.get();
			while( e ){
				if( LoopHasBreak( e->body, depth ) ) return true;
				e = e->elseStmt.get();
			}
		}
		for( const Case& c : s.cases ) if( LoopHasBreak( c.body, depth ) ) return true;
	}
	return false;
}

bool Compiler::StmtReturns( const Stmt& s ) const
{
	switch( s.kind ){
	case Stmt::Return:		return true;
	case Stmt::BlockStmt:	return AllPathsReturn( s.body );
	case Stmt::If:			return s.elseStmt && AllPathsReturn( s.body ) && StmtReturns( *s.elseStmt );
	case Stmt::Switch: {
		bool hasDefault = false;
		for( const Case& c : s.cases ){
			if( !c.value ) hasDefault = true;
			if( !AllPathsReturn( c.body ) ) return false;
		}
		return hasDefault;
	}
	case Stmt::Loop:		return !s.value && !LoopHasBreak( s.body, 0 );
	default:				return false;
	}
}

bool Compiler::AllPathsReturn( const Block& b ) const
{
	for( const StmtPtr& s : b.stmts ) if( StmtReturns( *s ) ) return true;
	return false;
}

void Compiler::GenBlock( const Block& b )
{
	PushScope();
	bool unreachable = false;
	for( const StmtPtr& s : b.stmts ){
		if( unreachable ){
			Warn( s->pos, "ここには到達しません" );
			unreachable = false;
		}
		GenStmt( *s );
		if( s->kind == Stmt::Return || s->kind == Stmt::Break || s->kind == Stmt::Continue ) unreachable = true;
	}
	PopScope();
}

void Compiler::GenAssign( const Stmt& s )
{
	const Expr& t = *s.target;
	TypeRef type;
	bool isLocal = false;
	uint16_t index = 0;

	if( t.kind == Expr::Name ){
		if( const Local* l = FindLocal( t.text ) ){
			if( l->branchDepth < m_branchDepth ){
				Err( t.pos, "branch の中から外側の let 変数 '" + t.text + "' には代入できません（branch ごとにコピーされるため。var を使ってください）" );
				return;
			}
			type = l->type; isLocal = true; index = l->slot;
		} else {
			auto it = m_vars.find( t.text );
			if( it == m_vars.end() ){ Err( t.pos, "'" + t.text + "' は宣言されていません" ); return; }
			type = it->second.type; index = it->second.index;
		}
	} else if( t.kind == Expr::Member ){
		if( M.FindEnum( t.owner ) ){ Err( t.pos, "enum の値には代入できません" ); return; }
		const MVar* v = M.IsBank( t.owner ) ? M.FindVar( t.owner, t.text ) : nullptr;
		if( !v ){ Err( t.pos, "'" + t.owner + "." + t.text + "' は共有変数ではありません" ); return; }
		type = v->type; index = SharedVarOf( *v );
	} else {
		Err( t.pos, "代入の左辺には変数を書いてください" );
		return;
	}

	auto load  = [&]() { if( isLocal ) W.LdLocal( index ); else W.LdVar( index ); };
	auto store = [&]() { if( isLocal ) W.StLocal( index ); else W.StVar( index ); };

	if( s.op == Tok::Assign ){
		if( GenExprAs( *s.value, type, "代入" ) ) store();
		return;
	}

	// 複合代入：x op= v → x = x op v
	Tok op;
	switch( s.op ){
	case Tok::PlusAssign:		op = Tok::Plus; break;
	case Tok::MinusAssign:		op = Tok::Minus; break;
	case Tok::StarAssign:		op = Tok::Star; break;
	case Tok::SlashAssign:		op = Tok::Slash; break;
	case Tok::PercentAssign:	op = Tok::Percent; break;
	case Tok::AmpAssign:		op = Tok::Amp; break;
	case Tok::PipeAssign:		op = Tok::Pipe; break;
	case Tok::CaretAssign:		op = Tok::Caret; break;
	case Tok::ShlAssign:		op = Tok::Shl; break;
	default:					op = Tok::Shr; break;
	}
	TypeRef vt = TypeOf( *s.value );
	TypeRef rt = BinaryType( op, type, vt );
	if( IsErr( rt ) || !Assignable( type, rt ) ){
		if( !IsErr( GenExpr( *s.value ) ) ) Err( s.pos, "この複合代入は " + type.Name() + " と " + vt.Name() + " には使えません" );
		return;
	}
	load();
	if( !GenExprAs( *s.value, rt.base == ATS_TYPE_FLOAT ? kFloat : vt, "右辺" ) ) return;
	bool isFloat = rt.base == ATS_TYPE_FLOAT;
	Op code = OP_NOP;
	switch( op ){
	case Tok::Plus:		code = isFloat ? OP_ADD_F : OP_ADD_I; break;
	case Tok::Minus:	code = isFloat ? OP_SUB_F : OP_SUB_I; break;
	case Tok::Star:		code = isFloat ? OP_MUL_F : OP_MUL_I; break;
	case Tok::Slash:	code = isFloat ? OP_DIV_F : OP_DIV_I; break;
	case Tok::Percent:	code = OP_MOD_I; break;
	case Tok::Amp:		code = OP_BAND; break;
	case Tok::Pipe:		code = OP_BOR; break;
	case Tok::Caret:	code = OP_BXOR; break;
	case Tok::Shl:		code = OP_SHL; break;
	default:			code = OP_SHR; break;
	}
	W.Op( code );
	store();
}

void Compiler::GenSwitch( const Stmt& s )
{
	TypeRef vt = GenExpr( *s.value );
	if( IsErr( vt ) ) return;
	if( vt.base != ATS_TYPE_INT && vt.base != ATS_TYPE_ENUM ){
		Err( s.value->pos, "switch の値は int か enum にしてください（" + vt.Name() + " です）" );
		return;
	}
	std::vector<std::pair<int32_t, Label>> cases;
	std::vector<Label> labels;
	Label def = W.NewLabel(), end = W.NewLabel();
	bool hasDefault = false;
	for( const Case& c : s.cases ){
		Label l = W.NewLabel();
		labels.push_back( l );
		if( !c.value ){
			if( hasDefault ) Err( c.pos, "default が 2 つあります" );
			hasDefault = true;
			def = l;
			continue;
		}
		uint64_t slot = 0;
		std::string str;
		if( !ConstExpr( *c.value, vt, &slot, &str ) ) continue;
		int32_t v = (int32_t)(uint32_t)slot;
		for( const auto& o : cases ) if( o.first == v ) Err( c.pos, "case の値 " + std::to_string( v ) + " が重複しています" );
		cases.emplace_back( v, l );
	}
	if( !hasDefault ) labels.push_back( def );
	W.Switch( cases, def );
	for( size_t i = 0; i < s.cases.size(); ++i ){
		W.Bind( labels[i] );
		if( m_opt.debugInfo ) W.Line( (uint32_t)s.cases[i].pos.line );
		GenBlock( s.cases[i].body );
		W.Jmp( end );
	}
	if( !hasDefault ) W.Bind( def );
	W.Bind( end );
}

void Compiler::GenLoop( const Stmt& s )
{
	LoopCtx ctx;
	ctx.brk         = W.NewLabel();
	ctx.cont        = W.NewLabel();
	ctx.branchDepth = m_branchDepth;
	Label top = W.NewLabel();

	if( s.value ){
		// loop( n ) { … }
		uint16_t limit = AllocTemp(), counter = AllocTemp();
		if( !GenExprAs( *s.value, kInt, "loop の回数" ) ) return;
		W.StLocal( limit );
		W.PushI( 0 ); W.StLocal( counter );
		W.Bind( top );
		W.LdLocal( counter ); W.LdLocal( limit ); W.Op( OP_GE_I ); W.Jnz( ctx.brk );
		m_loops.push_back( ctx );
		GenBlock( s.body );
		m_loops.pop_back();
		W.Bind( ctx.cont );
		W.LdLocal( counter ); W.PushI( 1 ); W.Op( OP_ADD_I ); W.StLocal( counter );
		W.Jmp( top );
		W.Bind( ctx.brk );
		return;
	}

	if( !BlockWaits( s.body ) && !LoopHasBreak( s.body, 0 ) )
		Err( s.pos, "待機も break もない無限ループです（ゲームが止まります）" );
	W.Bind( top );
	W.Bind( ctx.cont );
	m_loops.push_back( ctx );
	GenBlock( s.body );
	m_loops.pop_back();
	W.Jmp( top );
	W.Bind( ctx.brk );
}

void Compiler::GenParallel( const Stmt& s )
{
	const char* kw = s.race ? "race" : "parallel";
	if( s.branches.empty() ){ Err( s.pos, std::string( kw ) + " に branch がありません" ); return; }
	if( s.branches.size() == 1 ) Warn( s.pos, std::string( kw ) + " の branch が 1 つだけです" );

	std::vector<Label> starts;
	for( size_t i = 0; i < s.branches.size(); ++i ){
		starts.push_back( W.NewLabel() );
		W.Fork( starts.back() );
	}
	Label join = W.NewLabel();
	W.Jmp( join );
	for( size_t i = 0; i < s.branches.size(); ++i ){
		W.Bind( starts[i] );
		++m_branchDepth;
		GenBlock( s.branches[i] );
		--m_branchDepth;
		W.Op( OP_END );
	}
	W.Bind( join );
	W.Op( s.race ? OP_RACE : OP_JOIN );
}

void Compiler::GenFire( const Stmt& s )
{
	std::vector<std::string> names;
	std::vector<TypeRef> types;
	std::vector<const Literal*> defs;
	bool known = false;
	if( const MEvent* me = M.FindEvent( s.name ) ){
		known = true;
		for( const MParam& p : me->params ){ names.push_back( p.name ); types.push_back( p.type ); defs.push_back( &p.def ); }
	} else {
		auto it = m_events.find( s.name );
		if( it != m_events.end() ){
			known = true;
			for( const Param& p : it->second->params ){
				TypeRef t;
				M.ResolveType( p.typeName, &t );
				names.push_back( p.name ); types.push_back( t ); defs.push_back( nullptr );
			}
		}
	}
	if( s.args.size() > (size_t)kMaxParams ){ Err( s.pos, "fire の引数は 8 個までです" ); return; }

	if( known ){
		bool ok = true;
		std::vector<const Arg*> bound( types.size(), nullptr );
		size_t pos = 0;
		for( const Arg& a : s.args ){
			if( a.name.empty() ){
				if( pos >= types.size() ){ Err( a.pos, "イベント '" + s.name + "' の引数が多すぎます" ); ok = false; break; }
				bound[pos++] = &a;
			} else {
				size_t k = 0;
				while( k < names.size() && names[k] != a.name ) ++k;
				if( k == names.size() ){ Err( a.pos, "イベント '" + s.name + "' に引数 '" + a.name + "' はありません" ); ok = false; break; }
				bound[k] = &a;
			}
		}
		if( !ok ) return;
		for( size_t i = 0; i < types.size(); ++i ){
			if( bound[i] ){
				std::string label = "引数 '" + names[i] + "'";
				if( !GenExprAs( *bound[i]->value, types[i], label.c_str() ) ) return;
			} else if( defs[i] && defs[i]->present ){
				uint64_t slot = 0; std::string str;
				if( !ConstLiteral( *defs[i], types[i], &slot, &str, s.pos ) ) return;
				EmitConst( types[i], slot, str );
			} else {
				Err( s.pos, "イベント '" + s.name + "' の引数 '" + names[i] + "' がありません" );
				return;
			}
		}
		W.Fire( s.name, (uint8_t)types.size() );
		return;
	}

	Warn( s.pos, "イベント '" + s.name + "' はマニフェストにもこのファイルにもありません（引数は検査されません）" );
	for( const Arg& a : s.args ){
		if( !a.name.empty() ){ Err( a.pos, "未知のイベントに名前指定の引数は使えません" ); return; }
		if( IsErr( GenExpr( *a.value ) ) ) return;
	}
	W.Fire( s.name, (uint8_t)s.args.size() );
}

void Compiler::GenStmt( const Stmt& s )
{
	if( m_opt.debugInfo ) W.Line( (uint32_t)s.pos.line, s.node );

	switch( s.kind ){
	case Stmt::Let: {
		TypeRef type;
		if( !s.typeName.empty() ){
			if( !ResolveType( s.typeName, s.typePos, &type ) ) return;
		} else {
			type = TypeOf( *s.value );
			if( IsErr( type ) ){
				// 型の誤りを報告させる
				GenExpr( *s.value );
				return;
			}
			if( type.IsVoid() ){ Err( s.value->pos, "値を返さない呼び出しは let に入れられません" ); return; }
		}
		uint16_t slot;
		// 初期値の式の中で同じ名前を参照させないため、先に式を生成する
		if( !GenExprAs( *s.value, type, "let の初期値" ) ) return;
		if( DeclareLocal( s.name, type, s.pos, &slot ) ) W.StLocal( slot );
		else W.Op( OP_POP );
		return;
	}
	case Stmt::Assign:
		GenAssign( s );
		return;
	case Stmt::ExprStmt:
		if( s.value->kind != Expr::Call ){
			Err( s.value->pos, "式の値が使われていません" );
			return;
		}
		GenCall( *s.value, true );
		return;
	case Stmt::If: {
		Label els = W.NewLabel(), end = W.NewLabel();
		if( !GenExprAs( *s.value, kBool, "if の条件" ) ) return;
		W.Jz( els );
		GenBlock( s.body );
		if( s.elseStmt ){
			W.Jmp( end );
			W.Bind( els );
			if( s.elseStmt->kind == Stmt::BlockStmt ) GenBlock( s.elseStmt->body );
			else GenStmt( *s.elseStmt );
			W.Bind( end );
		} else {
			W.Bind( els );
		}
		return;
	}
	case Stmt::Switch:
		GenSwitch( s );
		return;
	case Stmt::Loop:
		GenLoop( s );
		return;
	case Stmt::Break:
	case Stmt::Continue: {
		const char* kw = s.kind == Stmt::Break ? "break" : "continue";
		if( m_loops.empty() ){ Err( s.pos, std::string( kw ) + " は loop の中でだけ使えます" ); return; }
		if( m_loops.back().branchDepth != m_branchDepth ){ Err( s.pos, std::string( kw ) + " で branch の外の loop には移れません" ); return; }
		W.Jmp( s.kind == Stmt::Break ? m_loops.back().brk : m_loops.back().cont );
		return;
	}
	case Stmt::Return:
		if( m_branchDepth > 0 ){ Err( s.pos, "branch の中では return できません" ); return; }
		if( m_cur->isEvent ){
			if( s.value ){ Err( s.value->pos, "イベントは値を返せません" ); return; }
			W.Op( OP_END );
			return;
		}
		if( m_curRet.IsVoid() ){
			if( s.value ){ Err( s.value->pos, "関数 '" + m_cur->name + "' は値を返しません" ); return; }
		} else {
			if( !s.value ){ Err( s.pos, "関数 '" + m_cur->name + "' は " + m_curRet.Name() + " を返します" ); return; }
			if( !GenExprAs( *s.value, m_curRet, "戻り値" ) ) return;
		}
		W.Op( OP_RET );
		return;
	case Stmt::Parallel:
		GenParallel( s );
		return;
	case Stmt::Wait:
		if( GenExprAs( *s.value, kFloat, "wait の秒数" ) ) W.Op( OP_SLEEP );
		return;
	case Stmt::WaitFrames:
		if( GenExprAs( *s.value, kInt, "wait frames のフレーム数" ) ) W.Op( OP_WAIT_FRAMES );
		return;
	case Stmt::Yield:
		W.Op( OP_YIELD );
		return;
	case Stmt::Fire:
		GenFire( s );
		return;
	case Stmt::ResetVars:
		if( m_vars.empty() ) Warn( s.pos, "このスクリプトには var がありません" );
		W.Op( OP_RESET_VARS );
		return;
	case Stmt::BlockStmt:
		GenBlock( s.body );
		return;
	}
}

void Compiler::GenFunc( const FuncDecl& fd )
{
	m_cur         = &fd;
	m_scopes.clear();
	m_loops.clear();
	m_nextSlot    = 0;
	m_maxSlot     = 0;
	m_branchDepth = 0;
	PushScope();

	uint16_t entry;
	std::vector<TypeRef> ptypes;
	if( fd.isEvent ){
		std::vector<ats_type> types;
		for( const Param& p : fd.params ){
			TypeRef t;
			ResolveType( p.typeName, p.pos, &t );
			ptypes.push_back( t );
			types.push_back( t.base );
		}
		m_curRet = kVoid;
		entry = W.BeginEvent( fd.name, types, (uint16_t)fd.params.size() );
	} else {
		const FnInfo& fi = m_fns[fd.name];
		ptypes   = fi.params;
		m_curRet = fi.ret;
		entry    = fi.entry;
		W.BeginFunction( entry );
	}
	for( size_t i = 0; i < fd.params.size(); ++i ){
		uint16_t slot;
		DeclareLocal( fd.params[i].name, ptypes[i], fd.params[i].pos, &slot );
	}

	GenBlock( fd.body );

	if( fd.isEvent ){
		W.Op( OP_END );
	} else if( m_curRet.IsVoid() || IsErr( m_curRet ) ){
		W.Op( OP_RET );
	} else {
		if( !AllPathsReturn( fd.body ) ) Err( fd.body.close, "関数 '" + fd.name + "' の最後に return がありません" );
		W.Op( OP_RET );	// 到達しない（検証器のために置く）
	}
	W.EndEntry();
	W.SetLocalCount( entry, m_maxSlot );
	PopScope();
	m_cur = nullptr;
}

bool Compiler::Run( const Script& s, CompileResult* out )
{
	if( s.scriptId.empty() ) return false;
	W = ProgramWriter( s.scriptId );
	W.SetManifestHash( M.hash );
	out->scriptId = s.scriptId;

	DeclareVars( s );
	DeclareFuncs( s );
	AnalyzeLatency();

	// 関数名とイベント名の衝突
	for( const auto& kv : m_events )
		if( m_fns.count( kv.first ) ) Err( kv.second->pos, "'" + kv.first + "' はイベントと関数の両方に使われています" );

	for( const FuncDecl& f : s.funcs ){
		if( !f.isEvent && !m_fns.count( f.name ) ) continue;	// 宣言で弾かれたもの
		if( !f.isEvent && m_fns[f.name].decl != &f ) continue;
		if( f.isEvent && m_events[f.name] != &f ) continue;
		GenFunc( f );
	}
	if( s.funcs.empty() ) Warn( s.scriptPos, "イベントがありません" );

	if( D.HasErrors() ) return false;
	std::string err;
	out->bytes = W.Build( &err );
	if( out->bytes.empty() ){
		D.Error( m_file, 0, 0, "内部エラー：" + err );
		return false;
	}
	return true;
}

}	// namespace

//=========================================================================
// 公開関数
//=========================================================================
bool CompileSource( const std::string& source, const std::string& file, const Manifest& manifest,
					Diagnostics& diag, CompileResult* out, const CompileOptions& options )
{
	out->bytes.clear();
	out->scriptId.clear();
	Script script;
	size_t before = diag.ErrorCount();
	Parse( source, file, &script, diag );
	if( diag.ErrorCount() != before ) return false;

	Compiler c( manifest, diag, file, options );
	return c.Run( script, out ) && diag.ErrorCount() == before;
}

}	// namespace compiler
}	// namespace ats
