/**************************************************************************/
/*!	\file	ats_gen.cpp
	\brief	マニフェストからのコード生成（atsc gen）
	\note
	C++ のヘッダーは「生成コードだけでコアに登録できる」ことを目標にする。
	シグネチャのハッシュはコンパイラと同じ MCommand::SignatureHash() を使う。
***************************************************************************/
#include "ats_compiler.h"

#include <cstdio>
#include <map>
#include <set>

namespace ats {
namespace compiler {

namespace {

//=========================================================================
// 共通
//=========================================================================
const char* const kCppKeywords[] = {
	"alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "const",
	"constexpr", "continue", "default", "delete", "do", "double", "else", "enum", "explicit", "export",
	"extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
	"namespace", "new", "noexcept", "not", "nullptr", "operator", "or", "private", "protected", "public",
	"register", "return", "short", "signed", "sizeof", "static", "struct", "switch", "template", "this",
	"throw", "true", "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void",
	"volatile", "while", "call", "user", "impl", "rt", "vm", "prog", "out", "count",
};

// C++ の識別子として安全な名前にする（予約語・引数名と衝突するものは _ を付ける）
std::string Ident( const std::string& s )
{
	std::string o;
	for( unsigned char c : s ){
		if( (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80 ) o += (char)c;
		else o += '_';
	}
	if( o.empty() || (o[0] >= '0' && o[0] <= '9') ) o = "_" + o;
	for( const char* k : kCppKeywords ) if( o == k ) return o + "_";
	return o;
}

std::string Hex32( uint32_t v ) { char b[16]; snprintf( b, sizeof(b), "0x%08xu", v ); return b; }
std::string Hex64( uint64_t v ) { char b[32]; snprintf( b, sizeof(b), "0x%016llxull", (unsigned long long)v ); return b; }

std::string CppStr( const std::string& s )
{
	std::string o = "\"";
	for( char c : s ){
		switch( c ){
		case '"':	o += "\\\""; break;
		case '\\':	o += "\\\\"; break;
		case '\n':	o += "\\n"; break;
		default:	o += c;
		}
	}
	return o + "\"";
}

// C++ の型（引数として受け取る型）
std::string CppType( const TypeRef& t )
{
	switch( t.base ){
	case ATS_TYPE_BOOL:		return "bool";
	case ATS_TYPE_INT:		return "int32_t";
	case ATS_TYPE_FLOAT:	return "float";
	case ATS_TYPE_STRING:	return "const char*";
	case ATS_TYPE_ENUM:		return Ident( t.enumName );
	case ATS_TYPE_HANDLE:	return "uint64_t";
	default:				return "void";
	}
}

// ats_call から引数を取り出す式
std::string ArgExpr( const TypeRef& t, size_t i )
{
	std::string idx = std::to_string( i );
	switch( t.base ){
	case ATS_TYPE_BOOL:		return "ats_arg_bool( call, " + idx + " ) != 0";
	case ATS_TYPE_INT:		return "ats_arg_int( call, " + idx + " )";
	case ATS_TYPE_FLOAT:	return "ats_arg_float( call, " + idx + " )";
	case ATS_TYPE_STRING:	return "ats_arg_string( call, " + idx + " )";
	case ATS_TYPE_ENUM:		return "static_cast<" + Ident( t.enumName ) + ">( ats_arg_int( call, " + idx + " ) )";
	case ATS_TYPE_HANDLE:	return "ats_arg_handle( call, " + idx + " )";
	default:				return "";
	}
}

const char* TypeConst( const TypeRef& t )
{
	switch( t.base ){
	case ATS_TYPE_BOOL:		return "ATS_TYPE_BOOL";
	case ATS_TYPE_INT:		return "ATS_TYPE_INT";
	case ATS_TYPE_FLOAT:	return "ATS_TYPE_FLOAT";
	case ATS_TYPE_STRING:	return "ATS_TYPE_STRING";
	case ATS_TYPE_ENUM:		return "ATS_TYPE_ENUM";
	case ATS_TYPE_HANDLE:	return "ATS_TYPE_HANDLE";
	default:				return "ATS_TYPE_VOID";
	}
}

std::string ParamList( const std::vector<MParam>& ps, bool leadingComma )
{
	std::string o;
	for( size_t i = 0; i < ps.size(); ++i ){
		if( i || leadingComma ) o += ", ";
		o += CppType( ps[i].type ) + " " + Ident( ps[i].name );
	}
	return o;
}

std::string ArgList( const std::vector<MParam>& ps, bool leadingComma )
{
	std::string o;
	for( size_t i = 0; i < ps.size(); ++i ){
		if( i || leadingComma ) o += ", ";
		o += ArgExpr( ps[i].type, i );
	}
	return o;
}

std::string Describe( const MCommand& c )
{
	std::string d = c.display.empty() ? c.name : c.display;
	if( !c.category.empty() ) d += "（" + c.category + "）";
	if( c.query ) d += " ［クエリ］";
	else if( c.latent ) d += " ［待機あり" + (c.channel.empty() ? std::string() : "・channel: " + c.channel) + "］";
	if( c.deprecated ) d += " ［非推奨］";
	return d;
}

std::string NamespaceOf( const Manifest& m, const GenOptions& opt )
{
	std::string ns = opt.nameSpace.empty() ? m.project : opt.nameSpace;
	if( ns.empty() ) ns = "atomscript_project";
	// "a::b" はそのまま、それ以外は識別子にする
	std::string out, part;
	for( size_t i = 0; i <= ns.size(); ++i ){
		if( i == ns.size() || (ns[i] == ':' && i + 1 < ns.size() && ns[i+1] == ':') ){
			if( !out.empty() ) out += "::";
			out += Ident( part );
			part.clear();
			if( i < ns.size() ) ++i;
		} else {
			part += ns[i];
		}
	}
	return out;
}

}	// namespace

//=========================================================================
// C++
//=========================================================================
std::string GenerateCpp( const Manifest& m, const GenOptions& opt )
{
	const std::string ns = NamespaceOf( m, opt );
	std::string o;
	auto L = [&]( const std::string& s ) { o += s; o += "\n"; };

	L( "/**************************************************************************/" );
	L( "/*!\t\\brief\tAtomScript 登録コード（atsc gen が " + (opt.source.empty() ? std::string( "マニフェスト" ) : opt.source) + " から生成）" );
	L( "\t\\note\t手で編集しない。マニフェストを変えたら atsc gen で作り直す。" );
	L( "***************************************************************************/" );
	L( "#pragma once" );
	L( "" );
	L( "#include \"atomscript/ats_api.h\"" );
	L( "" );
	L( "#include <cstdint>" );
	L( "" );
	L( "namespace " + ns + " {" );
	L( "" );
	L( "// 生成元のマニフェストのハッシュ（.atsb のヘッダーと比べられる）" );
	L( "constexpr uint64_t kManifestHash = " + Hex64( m.hash ) + ";" );
	L( "" );

	// enum ---------------------------------------------------------------
	if( !m.enums.empty() ){
		L( "//=========================================================================" );
		L( "// enum" );
		L( "//=========================================================================" );
		for( const MEnum& e : m.enums ){
			if( !e.display.empty() ) L( "// " + e.display );
			L( "enum class " + Ident( e.name ) + " : int32_t {" );
			for( const auto& v : e.values ) L( "\t" + Ident( v.first ) + " = " + std::to_string( v.second ) + "," );
			L( "};" );
			L( "" );
		}
	}

	// 値の作成 -------------------------------------------------------------
	L( "//=========================================================================" );
	L( "// ats_value の作成（待機ありコマンドの結果を ats_call_complete で返すときなどに使う）" );
	L( "//=========================================================================" );
	L( "inline ats_value MakeValue( bool v )        { ats_value x = {}; x.type = ATS_TYPE_BOOL;   x.v.b = v ? 1 : 0; return x; }" );
	L( "inline ats_value MakeValue( int32_t v )     { ats_value x = {}; x.type = ATS_TYPE_INT;    x.v.i = v; return x; }" );
	L( "inline ats_value MakeValue( float v )       { ats_value x = {}; x.type = ATS_TYPE_FLOAT;  x.v.f = v; return x; }" );
	L( "inline ats_value MakeValue( const char* v ) { ats_value x = {}; x.type = ATS_TYPE_STRING; x.v.s = v; return x; }" );
	L( "inline ats_value MakeHandle( uint64_t v )   { ats_value x = {}; x.type = ATS_TYPE_HANDLE; x.v.h = v; return x; }" );
	for( const MEnum& e : m.enums )
		L( "inline ats_value MakeValue( " + Ident( e.name ) + " v ) { ats_value x = {}; x.type = ATS_TYPE_ENUM; x.v.i = static_cast<int32_t>( v ); return x; }" );
	L( "" );

	// 共有変数 -----------------------------------------------------------
	if( !m.vars.empty() ){
		L( "//=========================================================================" );
		L( "// 共有変数の ID（ats_var_get / ats_var_set に渡す）" );
		L( "//=========================================================================" );
		L( "namespace vars {" );
		for( const std::string& bank : m.banks ){
			L( "\tnamespace " + Ident( bank ) + " {" );
			for( const MVar& v : m.vars ){
				if( v.bank != bank ) continue;
				L( "\t\tconstexpr uint32_t " + Ident( v.name ) + " = " + std::to_string( v.id ) + ";\t// " + v.type.Name() +
				   (v.scope == ATS_SCOPE_PERSISTENT ? "（セーブ対象）" : "（セーブしない）") );
			}
			L( "\t}" );
		}
		L( "}" );
		L( "" );
	}

	// イベント -----------------------------------------------------------
	if( !m.events.empty() ){
		L( "//=========================================================================" );
		L( "// イベントの発火" );
		L( "//=========================================================================" );
		L( "namespace events {" );
		for( const MEvent& e : m.events ){
			std::string args, setup;
			for( size_t i = 0; i < e.params.size(); ++i ){
				const MParam& p = e.params[i];
				std::string make = p.type.base == ATS_TYPE_HANDLE ? "MakeHandle" : "MakeValue";
				setup += (i ? ", " : "") + make + "( " + Ident( p.name ) + " )";
			}
			std::string params = ParamList( e.params, true );
			std::string n = Ident( e.name );
			L( "\tconstexpr const char* " + n + " = " + CppStr( e.name ) + ";" );
			if( e.params.empty() ){
				L( "\tinline ats_result Fire" + n + "( ats_vm* vm, ats_program* prog, ats_fiber_id* out = nullptr ) { return ats_vm_fire_event( vm, prog, " + n + ", nullptr, 0, out ); }" );
				L( "\tinline ats_result Broadcast" + n + "( ats_vm* vm, int* count = nullptr ) { return ats_vm_broadcast_event( vm, " + n + ", nullptr, 0, count ); }" );
			} else {
				std::string argc = std::to_string( e.params.size() );
				L( "\tinline ats_result Fire" + n + "( ats_vm* vm, ats_program* prog" + params + ", ats_fiber_id* out = nullptr ) {" );
				L( "\t\tconst ats_value args[] = { " + setup + " };" );
				L( "\t\treturn ats_vm_fire_event( vm, prog, " + n + ", args, " + argc + ", out );" );
				L( "\t}" );
				L( "\tinline ats_result Broadcast" + n + "( ats_vm* vm" + params + ", int* count = nullptr ) {" );
				L( "\t\tconst ats_value args[] = { " + setup + " };" );
				L( "\t\treturn ats_vm_broadcast_event( vm, " + n + ", args, " + argc + ", count );" );
				L( "\t}" );
			}
		}
		L( "}" );
		L( "" );
	}

	// 実装インターフェイス ---------------------------------------------
	L( "//=========================================================================" );
	L( "// コマンド・クエリの実装インターフェイス" );
	L( "//\t即時コマンド・クエリ：引数を受け取って結果を返す" );
	L( "//\t待機ありコマンド：ats_call* を受け取り、ATS_DONE / ATS_PENDING / ATS_RUNNING / ATS_FAIL を返す。" );
	L( "//\t\tATS_PENDING を返すときは ats_call_get_token( call ) でトークンを取り、終わったら" );
	L( "//\t\tats_call_complete( ats_call_vm( call ), token, 結果 ) を呼ぶ（結果がなければ nullptr）" );
	L( "//=========================================================================" );
	L( "class Commands {" );
	L( "public:" );
	L( "\tvirtual ~Commands() = default;" );
	L( "" );
	for( const MCommand& c : m.commands ){
		L( "\t// " + Describe( c ) );
		if( !c.description.empty() ) L( "\t// " + c.description );
		std::string ret = c.ret.IsVoid() ? "void" : (c.ret.base == ATS_TYPE_STRING ? "const char*" : CppType( c.ret ));
		if( !c.query && c.latent ){
			if( !c.ret.IsVoid() ) L( "\t// 結果：" + c.ret.Name() + "（ats_call_set_result か ats_call_complete で返す）" );
			L( "\tvirtual ats_status " + Ident( c.name ) + "( ats_call* call" + ParamList( c.params, true ) + " ) = 0;" );
		} else {
			if( c.ret.base == ATS_TYPE_STRING ) L( "\t// 戻り値の文字列は VM がコピーする（呼び出しから戻るまで有効なら一時バッファでよい）" );
			L( "\tvirtual " + ret + " " + Ident( c.name ) + "( " + ParamList( c.params, false ) + " ) = 0;" );
		}
		L( "" );
	}
	L( "\t// 待機中のコマンドが中断されたとき（ファイバの中断・race・VM の破棄）" );
	L( "\tvirtual void OnCancel( ats_vm* vm, ats_call_token token ) { (void)vm; (void)token; }" );
	L( "};" );
	L( "" );

	// 中継関数 -----------------------------------------------------------
	L( "namespace detail {" );
	for( const MCommand& c : m.commands ){
		std::string n = Ident( c.name );
		std::string self = "static_cast<Commands*>( user )";
		if( c.query ){
			std::string make = c.ret.base == ATS_TYPE_HANDLE ? "MakeHandle" : "MakeValue";
			L( "\tinline ats_result ATS_CALL Query_" + n + "( ats_call* call, void* user ) {" );
			L( "\t\tconst ats_value r = " + make + "( " + self + "->" + n + "( " + ArgList( c.params, false ) + " ) );" );
			L( "\t\treturn ats_call_set_result( call, &r );" );
			L( "\t}" );
			continue;
		}
		L( "\tinline ats_status ATS_CALL Command_" + n + "( ats_call* call, void* user ) {" );
		if( c.latent ){
			L( "\t\treturn " + self + "->" + n + "( call" + ArgList( c.params, true ) + " );" );
		} else if( c.ret.IsVoid() ){
			L( "\t\t" + self + "->" + n + "( " + ArgList( c.params, false ) + " );" );
			if( c.params.empty() ) L( "\t\t(void)call;" );
			L( "\t\treturn ATS_DONE;" );
		} else {
			std::string make = c.ret.base == ATS_TYPE_HANDLE ? "MakeHandle" : "MakeValue";
			L( "\t\tconst ats_value r = " + make + "( " + self + "->" + n + "( " + ArgList( c.params, false ) + " ) );" );
			L( "\t\tats_call_set_result( call, &r );" );
			L( "\t\treturn ATS_DONE;" );
		}
		L( "\t}" );
	}
	L( "\tinline void ATS_CALL Cancel( ats_vm* vm, ats_call_token token, void* user ) { static_cast<Commands*>( user )->OnCancel( vm, token ); }" );
	L( "}" );
	L( "" );

	// 登録 ---------------------------------------------------------------
	L( "//=========================================================================" );
	L( "// 登録（ランタイムを作った直後に 1 回呼ぶ。impl はランタイムより長生きさせる）" );
	L( "//=========================================================================" );
	L( "inline ats_result RegisterCommands( ats_runtime* rt, Commands* impl ) {" );
	L( "\tats_result r = ATS_OK;" );
	for( const MCommand& c : m.commands ){
		std::string n = Ident( c.name );
		if( c.query ){
			L( "\t{ ats_query_desc d = {}; d.size = sizeof(d); d.name = " + CppStr( c.name ) + "; d.sig_hash = " + Hex32( c.SignatureHash() ) +
			   "; d.fn = detail::Query_" + n + "; d.user = impl; if( (r = ats_register_query( rt, &d )) != ATS_OK ) return r; }" );
		} else {
			std::string channel = c.channel.empty() ? "nullptr" : CppStr( c.channel );
			L( "\t{ ats_command_desc d = {}; d.size = sizeof(d); d.name = " + CppStr( c.name ) + "; d.sig_hash = " + Hex32( c.SignatureHash() ) +
			   "; d.channel = " + channel + "; d.fn = detail::Command_" + n + "; d.cancel = detail::Cancel; d.user = impl; if( (r = ats_register_command( rt, &d )) != ATS_OK ) return r; }" );
		}
	}
	L( "\t(void)impl;" );
	L( "\treturn r;" );
	L( "}" );
	L( "" );

	L( "inline ats_result DefineVars( ats_runtime* rt ) {" );
	L( "\tats_result r = ATS_OK;" );
	for( const MVar& v : m.vars ){
		std::string init = "{}";
		if( v.init.present ){
			switch( v.type.base ){
			case ATS_TYPE_BOOL:		init = "MakeValue( " + std::string( v.init.text == "true" ? "true" : "false" ) + " )"; break;
			case ATS_TYPE_INT:		init = "MakeValue( static_cast<int32_t>( " + v.init.text + " ) )"; break;
			case ATS_TYPE_FLOAT:	init = "MakeValue( static_cast<float>( " + v.init.text + " ) )"; break;
			case ATS_TYPE_STRING:	init = "MakeValue( " + CppStr( v.init.text ) + " )"; break;
			case ATS_TYPE_ENUM: {
				std::string name = v.init.text;
				size_t dot = name.find( '.' );
				if( dot != std::string::npos ) name = name.substr( dot + 1 );
				init = "MakeValue( " + Ident( v.type.enumName ) + "::" + Ident( name ) + " )";
				break;
			}
			case ATS_TYPE_HANDLE:	init = "MakeHandle( 0 )"; break;
			default: break;
			}
		}
		L( "\t{ ats_var_desc d = {}; d.size = sizeof(d); d.id = " + std::to_string( v.id ) + "; d.name = " + CppStr( v.bank + "." + v.name ) +
		   "; d.type = " + TypeConst( v.type ) + "; d.scope = " + (v.scope == ATS_SCOPE_PERSISTENT ? "ATS_SCOPE_PERSISTENT" : "ATS_SCOPE_SESSION") +
		   "; d.init = " + init + "; if( (r = ats_define_var( rt, &d )) != ATS_OK ) return r; }" );
	}
	L( "\treturn r;" );
	L( "}" );
	L( "" );
	L( "// 共有変数とコマンド・クエリをまとめて登録する" );
	L( "inline ats_result Register( ats_runtime* rt, Commands* impl ) {" );
	L( "\tats_result r = DefineVars( rt );" );
	L( "\treturn r != ATS_OK ? r : RegisterCommands( rt, impl );" );
	L( "}" );
	L( "" );
	L( "}\t// namespace " + ns );
	return o;
}

//=========================================================================
// C#（Unity パッケージの AtomScript 名前空間の API を使う）
//=========================================================================
namespace {

const char* const kCsKeywords[] = {
	"abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const",
	"continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern",
	"false", "finally", "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int", "interface",
	"internal", "is", "lock", "long", "namespace", "new", "null", "object", "operator", "out", "override",
	"params", "private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed", "short",
	"sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true", "try", "typeof",
	"uint", "ulong", "unchecked", "unsafe", "ushort", "using", "virtual", "void", "volatile", "while",
};

// 生成コードの引数名・ローカル名と衝突させない名前
const char* const kCsReserved[] = { "ct", "call", "impl", "rt", "vm", "program", "count" };

// C# の識別子にする（使えない文字は _、予約語は @ を付ける）
std::string CsIdent( const std::string& s )
{
	std::string o;
	for( unsigned char c : s ){
		if( (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80 ) o += (char)c;
		else o += '_';
	}
	if( o.empty() || (o[0] >= '0' && o[0] <= '9') ) o = "_" + o;
	for( const char* k : kCsKeywords ) if( o == k ) return "@" + o;
	return o;
}

// snake_case → PascalCase（すでに PascalCase ならそのまま）
std::string Pascal( const std::string& s )
{
	std::string o;
	bool up = true;
	for( char c : s ){
		if( c == '_' || c == '-' || c == '.' || c == ' ' ){ up = true; continue; }
		o += up && c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
		up = false;
	}
	return CsIdent( o.empty() ? s : o );
}

// snake_case → camelCase（引数名）
std::string Camel( const std::string& s )
{
	std::string p = Pascal( s );
	if( p[0] == '@' ) p = p.substr( 1 );
	if( p[0] >= 'A' && p[0] <= 'Z' ) p[0] = (char)(p[0] - 'A' + 'a');
	for( const char* k : kCsReserved ) if( p == k ) return p + "_";
	return CsIdent( p );
}

std::string CsStr( const std::string& s ) { return CppStr( s ); }

std::string CsType( const TypeRef& t )
{
	switch( t.base ){
	case ATS_TYPE_BOOL:		return "bool";
	case ATS_TYPE_INT:		return "int";
	case ATS_TYPE_FLOAT:	return "float";
	case ATS_TYPE_STRING:	return "string";
	case ATS_TYPE_ENUM:		return CsIdent( t.enumName );
	case ATS_TYPE_HANDLE:	return "AtsHandle";
	default:				return "void";
	}
}

const char* CsTypeConst( const TypeRef& t )
{
	switch( t.base ){
	case ATS_TYPE_BOOL:		return "AtsType.Bool";
	case ATS_TYPE_INT:		return "AtsType.Int";
	case ATS_TYPE_FLOAT:	return "AtsType.Float";
	case ATS_TYPE_STRING:	return "AtsType.String";
	case ATS_TYPE_ENUM:		return "AtsType.Enum";
	case ATS_TYPE_HANDLE:	return "AtsType.Handle";
	default:				return "AtsType.Void";
	}
}

// AtsCall から引数を取り出す式
std::string CsArgExpr( const TypeRef& t, size_t i )
{
	std::string idx = std::to_string( i );
	switch( t.base ){
	case ATS_TYPE_BOOL:		return "call.ArgBool( " + idx + " )";
	case ATS_TYPE_INT:		return "call.ArgInt( " + idx + " )";
	case ATS_TYPE_FLOAT:	return "call.ArgFloat( " + idx + " )";
	case ATS_TYPE_STRING:	return "call.ArgString( " + idx + " )";
	case ATS_TYPE_ENUM:		return "(" + CsIdent( t.enumName ) + ")call.ArgInt( " + idx + " )";
	case ATS_TYPE_HANDLE:	return "call.ArgHandle( " + idx + " )";
	default:				return "";
	}
}

// 値 → AtsValue の式
std::string CsToValue( const TypeRef& t, const std::string& v )
{
	switch( t.base ){
	case ATS_TYPE_BOOL:		return "AtsValue.Bool( " + v + " )";
	case ATS_TYPE_INT:		return "AtsValue.Int( " + v + " )";
	case ATS_TYPE_FLOAT:	return "AtsValue.Float( " + v + " )";
	case ATS_TYPE_STRING:	return "AtsValue.String( " + v + " )";
	case ATS_TYPE_ENUM:		return "AtsValue.Enum( (int)" + v + " )";
	case ATS_TYPE_HANDLE:	return "AtsValue.Handle( " + v + " )";
	default:				return "AtsValue.Void";
	}
}

std::string CsParamList( const std::vector<MParam>& ps )
{
	std::string o;
	for( size_t i = 0; i < ps.size(); ++i ) o += (i ? ", " : "") + CsType( ps[i].type ) + " " + Camel( ps[i].name );
	return o;
}

std::string CsArgList( const std::vector<MParam>& ps )
{
	std::string o;
	for( size_t i = 0; i < ps.size(); ++i ) o += (i ? ", " : "") + CsArgExpr( ps[i].type, i );
	return o;
}

std::string XmlEsc( const std::string& s )
{
	std::string o;
	for( char c : s ){
		switch( c ){
		case '<':	o += "&lt;"; break;
		case '>':	o += "&gt;"; break;
		case '&':	o += "&amp;"; break;
		default:	o += c;
		}
	}
	return o;
}

// 初期値の AtsValue の式
std::string CsInit( const MVar& v )
{
	if( !v.init.present ) return "AtsValue.Void";
	switch( v.type.base ){
	case ATS_TYPE_BOOL:		return std::string( "AtsValue.Bool( " ) + (v.init.text == "true" ? "true" : "false") + " )";
	case ATS_TYPE_INT:		return "AtsValue.Int( " + v.init.text + " )";
	case ATS_TYPE_FLOAT: {
		std::string f = v.init.text;
		if( f.find_first_of( ".eE" ) == std::string::npos ) f += ".0";
		return "AtsValue.Float( " + f + "f )";
	}
	case ATS_TYPE_STRING:	return "AtsValue.String( " + CsStr( v.init.text ) + " )";
	case ATS_TYPE_ENUM: {
		std::string name = v.init.text;
		size_t dot = name.find( '.' );
		if( dot != std::string::npos ) name = name.substr( dot + 1 );
		return "AtsValue.Enum( (int)" + CsIdent( v.type.enumName ) + "." + CsIdent( name ) + " )";
	}
	case ATS_TYPE_HANDLE:	return "AtsValue.Handle( default )";
	default:				return "AtsValue.Void";
	}
}

}	// namespace

std::string GenerateCSharp( const Manifest& m, const GenOptions& opt )
{
	const std::string project = m.project.empty() ? std::string( "AtomScriptProject" ) : Pascal( m.project );
	std::string ns = opt.nameSpace;
	if( ns.empty() ) ns = project;
	else {
		// "a::b" も "a.b" も受け付ける
		std::string out, part;
		for( size_t i = 0; i <= ns.size(); ++i ){
			bool sep = i == ns.size() || ns[i] == '.' || (ns[i] == ':' && i + 1 < ns.size() && ns[i+1] == ':');
			if( sep ){
				if( !part.empty() ){ if( !out.empty() ) out += "."; out += CsIdent( part ); }
				part.clear();
				if( i < ns.size() && ns[i] == ':' ) ++i;
			} else {
				part += ns[i];
			}
		}
		ns = out;
	}
	const std::string iface = "I" + project + "Commands";

	std::string o;
	auto L = [&]( const std::string& s ) { o += s; o += "\n"; };

	L( "// <auto-generated>" );
	L( "// AtomScript 登録コード（atsc gen が " + (opt.source.empty() ? std::string( "マニフェスト" ) : opt.source) + " から生成）" );
	L( "// 手で編集しない。マニフェストを変えたら atsc gen で作り直す。" );
	L( "// </auto-generated>" );
	L( "using System.Threading;" );
	L( "using AtomScript;" );
	L( "using UnityEngine;" );
	L( "" );
	L( "namespace " + ns );
	L( "{" );

	// enum ---------------------------------------------------------------
	for( const MEnum& e : m.enums ){
		if( !e.display.empty() ) L( "\t/// <summary>" + XmlEsc( e.display ) + "</summary>" );
		L( "\tpublic enum " + CsIdent( e.name ) + " : int" );
		L( "\t{" );
		for( const auto& v : e.values ) L( "\t\t" + CsIdent( v.first ) + " = " + std::to_string( v.second ) + "," );
		L( "\t}" );
		L( "" );
	}

	// 共有変数 -----------------------------------------------------------
	if( !m.vars.empty() ){
		L( "\t/// <summary>共有変数（ScriptVM.Get / Set に渡す）</summary>" );
		L( "\tpublic static class Vars" );
		L( "\t{" );
		for( const std::string& bank : m.banks ){
			L( "\t\tpublic static class " + Pascal( bank ) );
			L( "\t\t{" );
			for( const MVar& v : m.vars ){
				if( v.bank != bank ) continue;
				L( "\t\t\t/// <summary>" + XmlEsc( v.bank + "." + v.name ) + "：" + v.type.Name() +
				   (v.scope == ATS_SCOPE_PERSISTENT ? "（セーブ対象）" : "（セーブしない）") + "</summary>" );
				L( "\t\t\tpublic static readonly VarId<" + CsType( v.type ) + "> " + Pascal( v.name ) + " = new VarId<" + CsType( v.type ) + ">( " +
				   std::to_string( v.id ) + " );" );
			}
			L( "\t\t}" );
		}
		L( "\t}" );
		L( "" );
	}

	// イベント -----------------------------------------------------------
	if( !m.events.empty() ){
		L( "\t/// <summary>イベントの発火</summary>" );
		L( "\tpublic static class Events" );
		L( "\t{" );
		for( const MEvent& e : m.events ){
			std::string n = CsIdent( e.name );
			std::string params, args;
			for( size_t i = 0; i < e.params.size(); ++i ){
				params += ", " + CsType( e.params[i].type ) + " " + Camel( e.params[i].name );
				args += (i ? ", " : "") + CsToValue( e.params[i].type, Camel( e.params[i].name ) );
			}
			std::string arr = e.params.empty() ? "null" : "new[] { " + args + " }";
			L( "\t\tpublic const string " + n + " = " + CsStr( e.name ) + ";" );
			L( "\t\tpublic static FiberId Fire" + n + "( ScriptVM vm, ScriptProgram program" + params + " )" );
			L( "\t\t\t=> vm.FireEvent( program, " + n + ", " + arr + " );" );
			L( "\t\tpublic static int Broadcast" + n + "( ScriptVM vm" + params + " )" );
			L( "\t\t\t=> vm.BroadcastEvent( " + n + ", " + arr + " );" );
		}
		L( "\t}" );
		L( "" );
	}

	// 実装インターフェイス ---------------------------------------------
	L( "\t/// <summary>" );
	L( "\t/// コマンド・クエリの実装。待機ありコマンドは Awaitable を返す（終わると VM に完了が伝わる）。" );
	L( "\t/// ファイバが中断されると ct がキャンセルされる。例外を投げるとコマンドの失敗になる。" );
	L( "\t/// </summary>" );
	L( "\tpublic interface " + iface );
	L( "\t{" );
	for( size_t ci = 0; ci < m.commands.size(); ++ci ){
		const MCommand& c = m.commands[ci];
		if( ci ) L( "" );
		L( "\t\t/// <summary>" + XmlEsc( Describe( c ) ) + (c.description.empty() ? "" : "。" + XmlEsc( c.description )) + "</summary>" );
		if( c.deprecated ) L( "\t\t[System.Obsolete]" );
		std::string params = CsParamList( c.params );
		if( !c.query && c.latent ){
			std::string ret = c.ret.IsVoid() ? "Awaitable" : "Awaitable<" + CsType( c.ret ) + ">";
			L( "\t\t" + ret + " " + CsIdent( c.name ) + "( " + params + (params.empty() ? "" : ", ") + "CancellationToken ct );" );
		} else {
			L( "\t\t" + CsType( c.ret ) + " " + CsIdent( c.name ) + (params.empty() ? "();" : "( " + params + " );") );
		}
	}
	L( "\t}" );
	L( "" );

	// 登録 ---------------------------------------------------------------
	L( "\t/// <summary>ランタイムへの登録（ランタイムを作った直後に 1 回呼ぶ）</summary>" );
	L( "\tpublic static class Registration" );
	L( "\t{" );
	L( "\t\t/// <summary>生成元のマニフェストのハッシュ（.atsb のヘッダーと比べられる）</summary>" );
	{
		char b[32];
		snprintf( b, sizeof(b), "0x%016llxUL", (unsigned long long)m.hash );
		L( "\t\tpublic const ulong ManifestHash = " + std::string( b ) + ";" );
	}
	L( "" );
	L( "\t\t/// <summary>共有変数とコマンド・クエリをまとめて登録する</summary>" );
	L( "\t\tpublic static void Register( ScriptRuntime rt, " + iface + " impl )" );
	L( "\t\t{" );
	L( "\t\t\tDefineVars( rt );" );
	L( "\t\t\tRegisterCommands( rt, impl );" );
	L( "\t\t}" );
	L( "" );
	L( "\t\tpublic static void RegisterCommands( ScriptRuntime rt, " + iface + " impl )" );
	L( "\t\t{" );
	for( const MCommand& c : m.commands ){
		std::string n = CsIdent( c.name );
		char hash[16];
		snprintf( hash, sizeof(hash), "0x%08xu", c.SignatureHash() );
		std::string args = CsArgList( c.params );
		std::string invoke = "impl." + n + (args.empty() ? "()" : "( " + args + " )");
		if( c.query ){
			L( "\t\t\trt.RegisterQuery( " + CsStr( c.name ) + ", " + hash + "," );
			L( "\t\t\t\tcall => call.SetResult( " + CsToValue( c.ret, invoke ) + " ) );" );
		} else if( c.latent ){
			std::string full = args + (args.empty() ? "" : ", ") + "call.CancellationToken";
			std::string conv = c.ret.IsVoid() ? "" : ", v => " + CsToValue( c.ret, "v" );
			L( "\t\t\trt.RegisterCommand( " + CsStr( c.name ) + ", " + hash + ", " + (c.channel.empty() ? "null" : CsStr( c.channel )) + "," );
			L( "\t\t\t\tcall => call.Await( impl." + n + "( " + full + " )" + conv + " ) );" );
		} else {
			L( "\t\t\trt.RegisterCommand( " + CsStr( c.name ) + ", " + hash + ", " + (c.channel.empty() ? "null" : CsStr( c.channel )) + "," );
			if( c.ret.IsVoid() ) L( "\t\t\t\tcall => { " + invoke + "; return AtsStatus.Done; } );" );
			else L( "\t\t\t\tcall => { call.SetResult( " + CsToValue( c.ret, invoke ) + " ); return AtsStatus.Done; } );" );
		}
	}
	L( "\t\t}" );
	L( "" );
	L( "\t\tpublic static void DefineVars( ScriptRuntime rt )" );
	L( "\t\t{" );
	for( const MVar& v : m.vars ){
		L( "\t\t\trt.DefineVar( " + std::to_string( v.id ) + ", " + CsStr( v.bank + "." + v.name ) + ", " + CsTypeConst( v.type ) + ", " +
		   (v.scope == ATS_SCOPE_PERSISTENT ? "AtsVarScope.Persistent" : "AtsVarScope.Session") + ", " + CsInit( v ) + " );" );
	}
	L( "\t\t}" );
	L( "\t}" );
	L( "}" );
	return o;
}

//=========================================================================
// HTML（プランナー向けのコマンド一覧）
//=========================================================================
namespace {

std::string Esc( const std::string& s )
{
	std::string o;
	for( char c : s ){
		switch( c ){
		case '<':	o += "&lt;"; break;
		case '>':	o += "&gt;"; break;
		case '&':	o += "&amp;"; break;
		case '"':	o += "&quot;"; break;
		default:	o += c;
		}
	}
	return o;
}

std::string Sig( const MCommand& c )
{
	std::string s = c.name + "(";
	for( size_t i = 0; i < c.params.size(); ++i ){
		const MParam& p = c.params[i];
		s += (i ? ", " : "") + p.name + ": " + p.type.Name();
		if( p.def.present ) s += " = " + (p.def.quoted ? "\"" + p.def.text + "\"" : p.def.text);
	}
	s += ")";
	if( !c.ret.IsVoid() ) s += " -> " + c.ret.Name();
	return s;
}

}	// namespace

std::string GenerateHtml( const Manifest& m, const GenOptions& opt )
{
	std::string o;
	auto L = [&]( const std::string& s ) { o += s; o += "\n"; };
	std::string title = (m.project.empty() ? std::string( "AtomScript" ) : m.project) + " コマンド一覧";

	L( "<!doctype html>" );
	L( "<html lang=\"ja\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">" );
	L( "<title>" + Esc( title ) + "</title>" );
	L( "<style>" );
	L( ":root{--fg:#1f2328;--muted:#59636e;--line:#d1d9e0;--bg:#fff;--tag:#eef1f4;--accent:#0969da}" );
	L( "@media (prefers-color-scheme:dark){:root{--fg:#e6edf3;--muted:#9198a1;--line:#3d444d;--bg:#0d1117;--tag:#262c36;--accent:#4493f8}}" );
	L( "body{font-family:system-ui,-apple-system,'Segoe UI','Yu Gothic UI',sans-serif;color:var(--fg);background:var(--bg);margin:0 auto;max-width:1040px;padding:24px 16px;line-height:1.6}" );
	L( "h1{font-size:24px}h2{font-size:19px;margin-top:32px;border-bottom:1px solid var(--line);padding-bottom:4px}h3{font-size:16px;margin:20px 0 4px}" );
	L( "code{font-family:Consolas,'Cascadia Code',monospace;font-size:13px}" );
	L( "table{border-collapse:collapse;width:100%;font-size:14px;margin:6px 0 12px}th,td{border:1px solid var(--line);padding:4px 8px;text-align:left;vertical-align:top}" );
	L( ".tag{display:inline-block;background:var(--tag);border-radius:4px;padding:0 6px;font-size:12px;margin-left:6px}.muted{color:var(--muted)}" );
	L( "</style></head><body>" );
	L( "<h1>" + Esc( title ) + "</h1>" );
	L( "<p class=\"muted\">atsc gen が " + Esc( opt.source.empty() ? std::string( "マニフェスト" ) : opt.source ) + " から生成。手で編集しない。</p>" );

	// コマンドをカテゴリごとに並べる
	std::vector<std::string> cats;
	for( const MCommand& c : m.commands ){
		std::string cat = c.category.empty() ? "その他" : c.category;
		bool seen = false;
		for( const std::string& x : cats ) if( x == cat ) seen = true;
		if( !seen ) cats.push_back( cat );
	}
	L( "<h2>コマンド・クエリ</h2>" );
	for( const std::string& cat : cats ){
		L( "<h3>" + Esc( cat ) + "</h3>" );
		L( "<table><tr><th>名前</th><th>書き方</th><th>種類</th><th>引数</th><th>説明</th></tr>" );
		for( const MCommand& c : m.commands ){
			if( (c.category.empty() ? "その他" : c.category) != cat ) continue;
			std::string kind = c.query ? "クエリ" : c.latent ? "待機あり（await）" : "即時";
			if( !c.channel.empty() ) kind += "<br><span class=\"muted\">channel: " + Esc( c.channel ) + "</span>";
			std::string params;
			for( const MParam& p : c.params ){
				params += "<code>" + Esc( p.name ) + "</code>: " + Esc( p.type.Name() );
				if( !p.display.empty() ) params += "（" + Esc( p.display ) + "）";
				if( p.def.present ) params += " <span class=\"muted\">既定 " + Esc( p.def.text ) + "</span>";
				params += "<br>";
			}
			std::string name = Esc( c.display.empty() ? c.name : c.display );
			if( c.deprecated ) name += "<span class=\"tag\">非推奨</span>";
			L( "<tr><td>" + name + "</td><td><code>" + Esc( (c.latent && !c.query ? "await " : "") + Sig( c ) ) + "</code></td><td>" + kind +
			   "</td><td>" + params + "</td><td>" + Esc( c.description ) + "</td></tr>" );
		}
		L( "</table>" );
	}

	if( !m.events.empty() ){
		L( "<h2>イベント</h2><table><tr><th>名前</th><th>引数</th></tr>" );
		for( const MEvent& e : m.events ){
			std::string params;
			for( size_t i = 0; i < e.params.size(); ++i ) params += (i ? ", " : "") + e.params[i].name + ": " + e.params[i].type.Name();
			L( "<tr><td><code>" + Esc( e.name ) + "</code></td><td><code>" + Esc( params ) + "</code></td></tr>" );
		}
		L( "</table>" );
	}

	if( !m.vars.empty() ){
		L( "<h2>共有変数</h2><table><tr><th>書き方</th><th>型</th><th>ID</th><th>セーブ</th><th>初期値</th></tr>" );
		for( const MVar& v : m.vars ){
			L( "<tr><td><code>" + Esc( v.bank + "." + v.name ) + "</code></td><td>" + Esc( v.type.Name() ) + "</td><td>" + std::to_string( v.id ) +
			   "</td><td>" + (v.scope == ATS_SCOPE_PERSISTENT ? "する" : "しない") + "</td><td>" + Esc( v.init.present ? v.init.text : "" ) + "</td></tr>" );
		}
		L( "</table>" );
	}

	if( !m.enums.empty() ){
		L( "<h2>enum</h2>" );
		for( const MEnum& e : m.enums ){
			std::string vals;
			for( const auto& v : e.values ) vals += "<code>" + Esc( e.name + "." + v.first ) + "</code> = " + std::to_string( v.second ) + "<br>";
			L( "<h3>" + Esc( e.name ) + (e.display.empty() ? "" : "（" + Esc( e.display ) + "）") + "</h3><p>" + vals + "</p>" );
		}
	}
	L( "</body></html>" );
	return o;
}

}	// namespace compiler
}	// namespace ats
