/**************************************************************************/
/*!	\file	ats_lsp.cpp
	\brief	言語サーバー（atsc lsp）
	\note
	対応：診断（エラー・警告）、補完、ホバー、定義へ移動、引数ヒント、アウトライン。
	マニフェストは設定（atomscript.manifest）か、文書のフォルダから上へ探した *.atsmanifest.yaml を使う。
***************************************************************************/
#include "ats_lsp.h"
#include "ats_compiler.h"
#include "ats_json.h"
#include "ats_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#if defined(_WIN32)
	#include <fcntl.h>
	#include <io.h>
#endif

namespace ats {
namespace compiler {

using json::Value;
namespace fs = std::filesystem;

namespace {

//=========================================================================
// URI とパス
//=========================================================================
std::string PercentDecode( const std::string& s )
{
	std::string o;
	for( size_t i = 0; i < s.size(); ++i ){
		if( s[i] == '%' && i + 2 < s.size() ){
			o += (char)std::strtol( s.substr( i + 1, 2 ).c_str(), nullptr, 16 );
			i += 2;
		} else {
			o += s[i];
		}
	}
	return o;
}

std::string UriToPath( const std::string& uri )
{
	std::string p = uri;
	if( p.compare( 0, 7, "file://" ) == 0 ) p = p.substr( 7 );
	p = PercentDecode( p );
	// "/e:/usr/..." → "e:/usr/..."
	if( p.size() >= 3 && p[0] == '/' && p[2] == ':' ) p = p.substr( 1 );
	return p;
}

std::string PathToUri( const std::string& path )
{
	std::string p = path;
	std::replace( p.begin(), p.end(), '\\', '/' );
	std::string o = "file://";
	if( !p.empty() && p[0] != '/' ) o += "/";
	const char* hex = "0123456789ABCDEF";
	for( unsigned char c : p ){
		bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
					c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
		if( keep ) o += (char)c;
		else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
	}
	return o;
}

// 比較用に正規化（区切りを / に、Windows のドライブ文字を小文字に）
std::string NormPath( const std::string& path )
{
	std::string p = path;
	std::replace( p.begin(), p.end(), '\\', '/' );
	if( p.size() >= 2 && p[1] == ':' ) p[0] = (char)std::tolower( (unsigned char)p[0] );
	return p;
}

//=========================================================================
// 文字列の位置（LSP は UTF-16 単位、コンパイラは文字＝コードポイント単位）
//=========================================================================
std::vector<std::string> SplitLines( const std::string& text )
{
	std::vector<std::string> lines;
	size_t b = 0;
	for( size_t i = 0; i <= text.size(); ++i ){
		if( i == text.size() || text[i] == '\n' ){
			std::string l = text.substr( b, i - b );
			if( !l.empty() && l.back() == '\r' ) l.pop_back();
			lines.push_back( l );
			b = i + 1;
		}
	}
	return lines;
}

size_t Utf8Len( unsigned char c )
{
	return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

// 行内のバイト位置 → UTF-16 の位置
int ByteToUtf16( const std::string& line, size_t byte )
{
	int u = 0;
	for( size_t i = 0; i < line.size() && i < byte; ){
		size_t n = Utf8Len( (unsigned char)line[i] );
		u += n == 4 ? 2 : 1;
		i += n;
	}
	return u;
}

// UTF-16 の位置 → 行内のバイト位置
size_t Utf16ToByte( const std::string& line, int u16 )
{
	int u = 0;
	size_t i = 0;
	while( i < line.size() && u < u16 ){
		size_t n = Utf8Len( (unsigned char)line[i] );
		u += n == 4 ? 2 : 1;
		i += n;
	}
	return std::min( i, line.size() );
}

// コードポイントの位置（1 始まり） → バイト位置
size_t CpToByte( const std::string& line, int cp1 )
{
	size_t i = 0;
	for( int c = 1; c < cp1 && i < line.size(); ++c ) i += Utf8Len( (unsigned char)line[i] );
	return std::min( i, line.size() );
}

bool IsIdentByte( unsigned char c )
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

Value LspPos( int line, int ch )
{
	Value p = Value::MakeObject();
	p.Set( "line", line );
	p.Set( "character", ch );
	return p;
}

Value Range( int l0, int c0, int l1, int c1 )
{
	Value r = Value::MakeObject();
	r.Set( "start", LspPos( l0, c0 ) );
	r.Set( "end", LspPos( l1, c1 ) );
	return r;
}

// コンパイラの (line, col)（1 始まり） → 単語の範囲
Value RangeAt( const std::vector<std::string>& lines, int line, int col )
{
	if( line <= 0 || (size_t)line > lines.size() ) return Range( 0, 0, 0, 1 );
	const std::string& l = lines[(size_t)line - 1];
	size_t b = CpToByte( l, col > 0 ? col : 1 );
	size_t e = b;
	while( e < l.size() && IsIdentByte( (unsigned char)l[e] ) ) ++e;
	if( e == b ) e = std::min( l.size(), b + Utf8Len( b < l.size() ? (unsigned char)l[b] : 'a' ) );
	if( e == b && b > 0 ){ b = 0; e = l.size(); }		// 行末など
	return Range( line - 1, ByteToUtf16( l, b ), line - 1, ByteToUtf16( l, e ) );
}

//=========================================================================
// 表示用の文字列
//=========================================================================
std::string ParamText( const MParam& p )
{
	std::string s = p.name + ": " + p.type.Name();
	if( p.def.present ) s += " = " + (p.def.quoted ? "\"" + p.def.text + "\"" : p.def.text);
	return s;
}

std::string CommandSig( const MCommand& c, bool withAwait )
{
	std::string s = (withAwait && c.latent && !c.query) ? "await " : "";
	s += c.name + "(";
	for( size_t i = 0; i < c.params.size(); ++i ) s += (i ? ", " : "") + ParamText( c.params[i] );
	s += ")";
	if( !c.ret.IsVoid() ) s += " -> " + c.ret.Name();
	return s;
}

std::string CommandDoc( const MCommand& c )
{
	std::string d;
	if( !c.display.empty() ) d += "**" + c.display + "**";
	if( !c.category.empty() ) d += (d.empty() ? "" : "　") + std::string( "（" ) + c.category + "）";
	d += "\n\n";
	d += c.query ? "クエリ（待機しない問い合わせ）" : c.latent ? "待機ありのコマンド（`await` が必要）" : "即時コマンド";
	if( !c.channel.empty() ) d += "　channel: `" + c.channel + "`";
	if( c.deprecated ) d += "\n\n⚠ 非推奨";
	if( !c.description.empty() ) d += "\n\n" + c.description;
	for( const MParam& p : c.params ){
		if( p.display.empty() ) continue;
		d += "\n\n- `" + p.name + "`：" + p.display;
	}
	return d;
}

std::string FuncSig( const FuncDecl& f )
{
	std::string s = (f.isEvent ? "event " : "fn ") + f.name + "(";
	for( size_t i = 0; i < f.params.size(); ++i ) s += (i ? ", " : "") + f.params[i].name + ": " + f.params[i].typeName;
	s += ")";
	if( !f.retType.empty() ) s += " -> " + f.retType;
	return s;
}

const char* const kKeywords[] = {
	"script", "var", "transient", "let", "event", "fn", "if", "else", "switch", "case", "default",
	"loop", "break", "continue", "return", "parallel", "race", "branch", "wait", "frames", "yield",
	"await", "fire", "reset", "vars", "true", "false",
	"bool", "int", "float", "string", "handle",
};

struct Builtin { const char* name; const char* sig; const char* doc; };
const Builtin kBuiltins[] = {
	{ "int",	"int(x) -> int",			"float を int に（切り捨て）、enum・bool を int に変換" },
	{ "float",	"float(x) -> float",		"int・enum を float に変換" },
	{ "rand",	"rand(n: int) -> int",		"0 以上 n 未満の乱数（VM のシード付き生成器。状態はセーブされる）" },
	{ "min",	"min(a, b)",				"小さい方（int / float）" },
	{ "max",	"max(a, b)",				"大きい方（int / float）" },
	{ "abs",	"abs(x)",					"絶対値（int / float）" },
};

// LSP の CompletionItemKind
enum { kKindMethod = 2, kKindFunction = 3, kKindVariable = 6, kKindModule = 9, kKindKeyword = 14,
	   kKindEnum = 13, kKindEnumMember = 20, kKindEvent = 23 };

}	// namespace

//=========================================================================
// 本体
//=========================================================================
struct LanguageServer::Impl {
	struct Document {
		std::string					uri;
		std::string					path;
		std::string					text;
		std::shared_ptr<Script>		ast;
		std::shared_ptr<Manifest>	manifest;
	};

	std::map<std::string, Document>	docs;
	std::string						rootPath;
	std::string						manifestSetting;	// 設定で指定されたマニフェスト
	std::set<std::string>			manifestDiagUris;	// 診断を出したマニフェストの URI
	bool							shutdown = false;
	bool							exit = false;

	//---------------------------------------------------------------------
	// 送信
	//---------------------------------------------------------------------
	static std::string Response( const Value& id, Value result )
	{
		Value v = Value::MakeObject();
		v.Set( "jsonrpc", "2.0" );
		v.Set( "id", id );
		v.Set( "result", std::move( result ) );
		return v.Dump();
	}
	static std::string ErrorResponse( const Value& id, int code, const std::string& msg )
	{
		Value e = Value::MakeObject();
		e.Set( "code", code );
		e.Set( "message", msg );
		Value v = Value::MakeObject();
		v.Set( "jsonrpc", "2.0" );
		v.Set( "id", id );
		v.Set( "error", std::move( e ) );
		return v.Dump();
	}
	static std::string Notification( const char* method, Value params )
	{
		Value v = Value::MakeObject();
		v.Set( "jsonrpc", "2.0" );
		v.Set( "method", method );
		v.Set( "params", std::move( params ) );
		return v.Dump();
	}

	//---------------------------------------------------------------------
	// マニフェストを探す
	//---------------------------------------------------------------------
	std::string FindManifest( const std::string& docPath ) const
	{
		std::error_code ec;
		if( !manifestSetting.empty() ){
			fs::path p = fs::u8path( manifestSetting );
			if( p.is_relative() && !rootPath.empty() ) p = fs::u8path( rootPath ) / p;
			return p.u8string();
		}
		fs::path dir = fs::u8path( docPath ).parent_path();
		for( int depth = 0; depth < 32 && !dir.empty(); ++depth ){
			std::vector<std::string> found;
			for( fs::directory_iterator it( dir, ec ), end; !ec && it != end; it.increment( ec ) ){
				std::string name = it->path().filename().u8string();
				if( name.size() > 17 && name.compare( name.size() - 17, 17, ".atsmanifest.yaml" ) == 0 ) found.push_back( it->path().u8string() );
			}
			if( !found.empty() ){
				std::sort( found.begin(), found.end() );
				return found[0];
			}
			fs::path parent = dir.parent_path();
			if( parent == dir ) break;
			dir = parent;
		}
		return "";
	}

	//---------------------------------------------------------------------
	// 診断
	//---------------------------------------------------------------------
	static Value ToLspDiag( const Diagnostic& d, const std::vector<std::string>& lines )
	{
		Value v = Value::MakeObject();
		v.Set( "range", RangeAt( lines, d.line, d.col ) );
		v.Set( "severity", d.severity == Diagnostic::Error ? 1 : 2 );
		v.Set( "source", "atsc" );
		v.Set( "message", d.message );
		return v;
	}

	static std::string ReadFile( const std::string& path )
	{
		std::ifstream f( fs::u8path( path ), std::ios::binary );
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}

	std::vector<std::string> Validate( Document& doc )
	{
		std::vector<std::string> out;
		const std::vector<std::string> lines = SplitLines( doc.text );

		// マニフェスト（毎回読み直す。小さいので十分速い）
		std::string mpath = FindManifest( doc.path );
		Diagnostics mdiag;
		auto man = std::make_shared<Manifest>();
		bool manOk = false;
		if( !mpath.empty() ) manOk = man->Load( mpath, mdiag );

		// 構文木（補完・アウトライン用。エラーがあっても途中まで作られる）
		auto script = std::make_shared<Script>();
		Diagnostics pdiag;
		Parse( doc.text, doc.path, script.get(), pdiag );
		doc.ast      = script;
		doc.manifest = man;

		Diagnostics cdiag;
		if( mpath.empty() ){
			cdiag = pdiag;
			cdiag.Warning( doc.path, 1, 1, "マニフェスト（*.atsmanifest.yaml）が見つかりません。設定 atomscript.manifest で指定するか、このファイルのフォルダか上のフォルダに置いてください" );
		} else if( !manOk ){
			cdiag = pdiag;
			cdiag.Error( doc.path, 1, 1, "マニフェストにエラーがあります：" + mpath );
		} else {
			CompileResult r;
			CompileSource( doc.text, doc.path, *man, cdiag, &r );
		}

		Value arr = Value::MakeArray();
		for( const Diagnostic& d : cdiag.List() )
			if( NormPath( d.file ) == NormPath( doc.path ) ) arr.Push( ToLspDiag( d, lines ) );
		Value p = Value::MakeObject();
		p.Set( "uri", doc.uri );
		p.Set( "diagnostics", std::move( arr ) );
		out.push_back( Notification( "textDocument/publishDiagnostics", std::move( p ) ) );

		// マニフェストの診断（ファイルごと。直ったものは空で送り直す）
		std::map<std::string, Value> byFile;
		std::map<std::string, std::vector<std::string>> fileLines;
		for( const Diagnostic& d : mdiag.List() ){
			std::string uri = PathToUri( d.file );
			if( !fileLines.count( uri ) ) fileLines[uri] = SplitLines( ReadFile( d.file ) );
			if( !byFile.count( uri ) ) byFile[uri] = Value::MakeArray();
			byFile[uri].Push( ToLspDiag( d, fileLines[uri] ) );
		}
		for( const std::string& uri : manifestDiagUris )
			if( !byFile.count( uri ) ) byFile[uri] = Value::MakeArray();
		manifestDiagUris.clear();
		for( auto& kv : byFile ){
			if( kv.second.Size() ) manifestDiagUris.insert( kv.first );
			Value mp = Value::MakeObject();
			mp.Set( "uri", kv.first );
			mp.Set( "diagnostics", kv.second );
			out.push_back( Notification( "textDocument/publishDiagnostics", std::move( mp ) ) );
		}
		return out;
	}

	std::vector<std::string> ValidateAll()
	{
		std::vector<std::string> out;
		for( auto& kv : docs ){
			auto v = Validate( kv.second );
			out.insert( out.end(), v.begin(), v.end() );
		}
		return out;
	}

	//---------------------------------------------------------------------
	// カーソル位置の単語
	//---------------------------------------------------------------------
	struct WordAt {
		std::string	word;
		std::string	owner;		// "Face.Smile" の Face
		int			line = 0;	// 0 始まり
		int			start = 0;	// UTF-16
		int			end = 0;
	};

	static WordAt GetWord( const Document& doc, const Value& pos, bool upToCursor )
	{
		WordAt w;
		std::vector<std::string> lines = SplitLines( doc.text );
		int line = pos["line"].AsInt();
		if( line < 0 || (size_t)line >= lines.size() ) return w;
		const std::string& l = lines[(size_t)line];
		size_t cur = Utf16ToByte( l, pos["character"].AsInt() );
		size_t b = cur, e = cur;
		while( b > 0 && IsIdentByte( (unsigned char)l[b - 1] ) ) --b;
		// UTF-8 の途中で止まらないように
		while( b < l.size() && ((unsigned char)l[b] & 0xC0) == 0x80 ) ++b;
		if( !upToCursor ) while( e < l.size() && IsIdentByte( (unsigned char)l[e] ) ) ++e;
		w.word  = l.substr( b, e - b );
		w.line  = line;
		w.start = ByteToUtf16( l, b );
		w.end   = ByteToUtf16( l, e );
		if( b > 0 && l[b - 1] == '.' ){
			size_t ob = b - 1;
			while( ob > 0 && IsIdentByte( (unsigned char)l[ob - 1] ) ) --ob;
			w.owner = l.substr( ob, b - 1 - ob );
		}
		return w;
	}

	// カーソルの前の文字列（同じ行）
	static std::string LineBefore( const Document& doc, const Value& pos )
	{
		std::vector<std::string> lines = SplitLines( doc.text );
		int line = pos["line"].AsInt();
		if( line < 0 || (size_t)line >= lines.size() ) return "";
		const std::string& l = lines[(size_t)line];
		return l.substr( 0, Utf16ToByte( l, pos["character"].AsInt() ) );
	}

	// カーソル行を含む関数（イベント）
	static const FuncDecl* FuncAt( const Document& doc, int line1 )
	{
		if( !doc.ast ) return nullptr;
		for( const FuncDecl& f : doc.ast->funcs )
			if( f.pos.line <= line1 && (f.body.close.line == 0 || line1 <= f.body.close.line) ) return &f;
		return nullptr;
	}

	static void CollectLets( const Block& b, int line1, std::vector<const Stmt*>& out )
	{
		for( const StmtPtr& s : b.stmts ){
			if( s->pos.line >= line1 ) break;
			if( s->kind == Stmt::Let ) out.push_back( s.get() );
			CollectLets( s->body, line1, out );
			const Stmt* e = s->elseStmt.get();
			while( e ){ CollectLets( e->body, line1, out ); e = e->elseStmt.get(); }
			for( const Case& c : s->cases ) CollectLets( c.body, line1, out );
			for( const Block& br : s->branches ) CollectLets( br, line1, out );
		}
	}

	//---------------------------------------------------------------------
	// 補完
	//---------------------------------------------------------------------
	static Value Item( const std::string& label, int kind, const std::string& detail, const std::string& doc = "" )
	{
		Value v = Value::MakeObject();
		v.Set( "label", label );
		v.Set( "kind", kind );
		if( !detail.empty() ) v.Set( "detail", detail );
		if( !doc.empty() ){
			Value d = Value::MakeObject();
			d.Set( "kind", "markdown" );
			d.Set( "value", doc );
			v.Set( "documentation", std::move( d ) );
		}
		return v;
	}

	Value Completion( const Document& doc, const Value& pos )
	{
		Value items = Value::MakeArray();
		const Manifest* m = doc.manifest.get();
		WordAt w = GetWord( doc, pos, true );

		// Enum. / bank. の後
		if( !w.owner.empty() ){
			if( m ){
				if( const MEnum* e = m->FindEnum( w.owner ) ){
					for( const auto& v : e->values )
						items.Push( Item( v.first, kKindEnumMember, e->name + "." + v.first + " = " + std::to_string( v.second ) ) );
				} else if( m->IsBank( w.owner ) ){
					for( const MVar& v : m->vars ){
						if( v.bank != w.owner ) continue;
						items.Push( Item( v.name, kKindVariable, v.type.Name() + "（id " + std::to_string( v.id ) + "）",
										  v.scope == ATS_SCOPE_PERSISTENT ? "セーブ対象の共有変数" : "セーブしない共有変数" ) );
					}
				}
			}
			return items;
		}

		// fire の後はイベント名
		std::string before = LineBefore( doc, pos );
		before = before.substr( 0, before.size() - w.word.size() );
		while( !before.empty() && before.back() == ' ' ) before.pop_back();
		if( before.size() >= 4 && before.compare( before.size() - 4, 4, "fire" ) == 0 ){
			std::set<std::string> seen;
			if( m ) for( const MEvent& e : m->events ){ seen.insert( e.name ); items.Push( Item( e.name, kKindEvent, "マニフェストのイベント" ) ); }
			if( doc.ast ) for( const FuncDecl& f : doc.ast->funcs )
				if( f.isEvent && !seen.count( f.name ) ) items.Push( Item( f.name, kKindEvent, FuncSig( f ) ) );
			return items;
		}
		const bool afterAwait = before.size() >= 5 && before.compare( before.size() - 5, 5, "await" ) == 0;

		// コマンド・クエリ（日本語の表示名でも絞り込めるようにする）
		if( m ){
			for( const MCommand& c : m->commands ){
				Value it = Item( c.name, c.query ? kKindMethod : kKindFunction, CommandSig( c, true ), CommandDoc( c ) );
				it.Set( "filterText", c.name + " " + c.display );
				// 引数の雛形（既定値のない引数だけ）
				std::string snippet = (c.latent && !c.query && !afterAwait) ? "await " : "";
				snippet += c.name + "(";
				int n = 0;
				for( const MParam& p : c.params ){
					if( p.def.present ) continue;
					if( n ) snippet += ", ";
					++n;
					snippet += "${" + std::to_string( n ) + ":" + p.name + "}";
				}
				snippet += ")";
				it.Set( "insertText", snippet );
				it.Set( "insertTextFormat", 2 );
				if( c.deprecated ){ Value tags = Value::MakeArray(); tags.Push( 1 ); it.Set( "tags", std::move( tags ) ); }
				items.Push( std::move( it ) );
			}
			for( const MEnum& e : m->enums ) items.Push( Item( e.name, kKindEnum, "enum", e.display ) );
			for( const std::string& b : m->banks ) items.Push( Item( b, kKindModule, "共有変数のバンク" ) );
		}
		for( const Builtin& b : kBuiltins ) items.Push( Item( b.name, kKindFunction, b.sig, b.doc ) );
		for( const char* k : kKeywords ) items.Push( Item( k, kKindKeyword, "" ) );

		// このファイルの var・関数、カーソルの関数の引数と let
		if( doc.ast ){
			for( const VarDecl& v : doc.ast->vars )
				items.Push( Item( v.name, kKindVariable, std::string( v.transient ? "transient " : "" ) + "var " + v.name + ": " + v.typeName ) );
			for( const FuncDecl& f : doc.ast->funcs )
				if( !f.isEvent ) items.Push( Item( f.name, kKindFunction, FuncSig( f ) ) );
			int line1 = pos["line"].AsInt() + 1;
			if( const FuncDecl* f = FuncAt( doc, line1 ) ){
				for( const Param& p : f->params ) items.Push( Item( p.name, kKindVariable, p.name + ": " + p.typeName + "（引数）" ) );
				std::vector<const Stmt*> lets;
				CollectLets( f->body, line1, lets );
				for( const Stmt* s : lets ) items.Push( Item( s->name, kKindVariable, "let " + s->name + (s->typeName.empty() ? "" : ": " + s->typeName) ) );
			}
		}
		return items;
	}

	//---------------------------------------------------------------------
	// ホバー
	//---------------------------------------------------------------------
	Value Hover( const Document& doc, const Value& pos )
	{
		WordAt w = GetWord( doc, pos, false );
		if( w.word.empty() ) return Value();
		const Manifest* m = doc.manifest.get();
		std::string md;

		if( !w.owner.empty() && m ){
			if( const MEnum* e = m->FindEnum( w.owner ) ){
				int32_t v;
				if( e->Find( w.word, &v ) ) md = "```ats\n" + e->name + "." + w.word + " = " + std::to_string( v ) + "\n```" + (e->display.empty() ? "" : "\n\n" + e->display);
			} else if( const MVar* v = m->FindVar( w.owner, w.word ) ){
				md = "```ats\n" + v->bank + "." + v->name + ": " + v->type.Name() + "\n```\n\n共有変数（id " + std::to_string( v->id ) + "、" +
					 (v->scope == ATS_SCOPE_PERSISTENT ? "セーブ対象" : "セーブしない") + "）" +
					 (v->init.present ? "\n\n初期値：`" + v->init.text + "`" : "");
			}
		}
		if( md.empty() && w.owner.empty() ){
			if( m ){
				if( const MCommand* c = m->FindCommand( w.word ) ) md = "```ats\n" + CommandSig( *c, true ) + "\n```\n\n" + CommandDoc( *c );
				else if( const MEnum* e = m->FindEnum( w.word ) ){
					md = "```ats\nenum " + e->name + "\n```" + (e->display.empty() ? "" : "\n\n" + e->display) + "\n";
					for( const auto& v : e->values ) md += "\n- `" + v.first + "` = " + std::to_string( v.second );
				} else if( m->IsBank( w.word ) ) md = "共有変数のバンク `" + w.word + "`";
				else if( const MEvent* ev = m->FindEvent( w.word ) ){
					std::string s = "event " + ev->name + "(";
					for( size_t i = 0; i < ev->params.size(); ++i ) s += (i ? ", " : "") + ParamText( ev->params[i] );
					md = "```ats\n" + s + ")\n```\n\nマニフェストのイベント";
				}
			}
			if( md.empty() ) for( const Builtin& b : kBuiltins ) if( w.word == b.name ) md = "```ats\n" + std::string( b.sig ) + "\n```\n\n組み込み関数：" + b.doc;
			if( md.empty() && doc.ast ){
				for( const VarDecl& v : doc.ast->vars )
					if( v.name == w.word ) md = "```ats\n" + std::string( v.transient ? "transient " : "" ) + "var " + v.name + ": " + v.typeName + "\n```\n\nスクリプト変数" + (v.transient ? "（セーブしない）" : "（セーブ対象）");
				for( const FuncDecl& f : doc.ast->funcs ) if( f.name == w.word ) md = "```ats\n" + FuncSig( f ) + "\n```";
			}
		}
		if( md.empty() ) return Value();
		Value contents = Value::MakeObject();
		contents.Set( "kind", "markdown" );
		contents.Set( "value", md );
		Value h = Value::MakeObject();
		h.Set( "contents", std::move( contents ) );
		h.Set( "range", Range( w.line, w.start, w.line, w.end ) );
		return h;
	}

	//---------------------------------------------------------------------
	// 定義へ移動
	//---------------------------------------------------------------------
	static Value Location( const std::string& uri, int line1 )
	{
		Value v = Value::MakeObject();
		v.Set( "uri", uri );
		int l = line1 > 0 ? line1 - 1 : 0;
		v.Set( "range", Range( l, 0, l, 0 ) );
		return v;
	}

	Value Definition( const Document& doc, const Value& pos )
	{
		WordAt w = GetWord( doc, pos, false );
		if( w.word.empty() ) return Value();
		const Manifest* m = doc.manifest.get();
		if( !w.owner.empty() ){
			if( m ){
				if( const MVar* v = m->FindVar( w.owner, w.word ) ) return Location( PathToUri( v->file ), v->line );
				if( const MEnum* e = m->FindEnum( w.owner ) ) return Location( PathToUri( e->file ), e->line );
			}
			return Value();
		}
		if( doc.ast ){
			for( const VarDecl& v : doc.ast->vars ) if( v.name == w.word ) return Location( doc.uri, v.pos.line );
			for( const FuncDecl& f : doc.ast->funcs ) if( f.name == w.word ) return Location( doc.uri, f.pos.line );
		}
		if( m ){
			if( const MCommand* c = m->FindCommand( w.word ) ) return Location( PathToUri( c->file ), c->line );
			if( const MEnum* e = m->FindEnum( w.word ) ) return Location( PathToUri( e->file ), e->line );
			if( const MEvent* e = m->FindEvent( w.word ) ) return Location( PathToUri( e->file ), e->line );
		}
		return Value();
	}

	//---------------------------------------------------------------------
	// 引数ヒント
	//---------------------------------------------------------------------
	Value SignatureHelp( const Document& doc, const Value& pos )
	{
		// カーソルより前を逆にたどり、閉じていない '(' を探す
		std::vector<std::string> lines = SplitLines( doc.text );
		int line = pos["line"].AsInt();
		if( line < 0 || (size_t)line >= lines.size() ) return Value();
		std::string text;
		for( int i = std::max( 0, line - 20 ); i < line; ++i ) text += lines[(size_t)i] + "\n";
		text += LineBefore( doc, pos );

		int depth = 0, commas = 0;
		bool inStr = false;
		size_t open = std::string::npos;
		for( size_t i = text.size(); i-- > 0; ){
			char c = text[i];
			if( c == '"' ){ inStr = !inStr; continue; }
			if( inStr ) continue;
			if( c == ')' ) ++depth;
			else if( c == '(' ){ if( depth == 0 ){ open = i; break; } --depth; }
			else if( c == ',' && depth == 0 ) ++commas;
			else if( c == '{' || c == '}' ) return Value();
		}
		if( open == std::string::npos ) return Value();
		size_t e = open, b = open;
		while( b > 0 && IsIdentByte( (unsigned char)text[b - 1] ) ) --b;
		std::string name = text.substr( b, e - b );
		if( name.empty() ) return Value();

		std::string label;
		std::vector<std::string> params;
		std::string docText;
		const Manifest* m = doc.manifest.get();
		if( m && m->FindCommand( name ) ){
			const MCommand* c = m->FindCommand( name );
			label = CommandSig( *c, false );
			for( const MParam& p : c->params ) params.push_back( ParamText( p ) );
			docText = CommandDoc( *c );
		} else if( doc.ast ){
			for( const FuncDecl& f : doc.ast->funcs ){
				if( f.name != name ) continue;
				label = FuncSig( f );
				for( const Param& p : f.params ) params.push_back( p.name + ": " + p.typeName );
			}
			if( label.empty() && m ){
				if( const MEvent* ev = m->FindEvent( name ) ){
					label = "event " + ev->name + "(";
					for( size_t i = 0; i < ev->params.size(); ++i ){ params.push_back( ParamText( ev->params[i] ) ); label += (i ? ", " : "") + params.back(); }
					label += ")";
				}
			}
		}
		if( label.empty() ) return Value();

		Value sig = Value::MakeObject();
		sig.Set( "label", label );
		if( !docText.empty() ){
			Value d = Value::MakeObject();
			d.Set( "kind", "markdown" );
			d.Set( "value", docText );
			sig.Set( "documentation", std::move( d ) );
		}
		Value ps = Value::MakeArray();
		for( const std::string& p : params ){
			Value pi = Value::MakeObject();
			pi.Set( "label", p );
			ps.Push( std::move( pi ) );
		}
		sig.Set( "parameters", std::move( ps ) );
		Value sigs = Value::MakeArray();
		sigs.Push( std::move( sig ) );
		Value r = Value::MakeObject();
		r.Set( "signatures", std::move( sigs ) );
		r.Set( "activeSignature", 0 );
		r.Set( "activeParameter", commas );
		return r;
	}

	//---------------------------------------------------------------------
	// アウトライン
	//---------------------------------------------------------------------
	Value DocumentSymbols( const Document& doc )
	{
		Value arr = Value::MakeArray();
		if( !doc.ast ) return arr;
		std::vector<std::string> lines = SplitLines( doc.text );
		auto sym = [&]( const std::string& name, const std::string& detail, int kind, int l0, int l1 ){
			Value s = Value::MakeObject();
			s.Set( "name", name );
			if( !detail.empty() ) s.Set( "detail", detail );
			s.Set( "kind", kind );
			int a = std::max( 0, l0 - 1 ), b = std::max( a, l1 - 1 );
			int endCh = (size_t)b < lines.size() ? ByteToUtf16( lines[(size_t)b], lines[(size_t)b].size() ) : 0;
			s.Set( "range", Range( a, 0, b, endCh ) );
			s.Set( "selectionRange", Range( a, 0, a, 0 ) );
			return s;
		};
		for( const VarDecl& v : doc.ast->vars ) arr.Push( sym( v.name, v.typeName, 13, v.pos.line, v.pos.line ) );
		for( const FuncDecl& f : doc.ast->funcs )
			arr.Push( sym( f.name, FuncSig( f ), f.isEvent ? 24 : 12, f.pos.line, f.body.close.line ? f.body.close.line : f.pos.line ) );
		return arr;
	}

	//---------------------------------------------------------------------
	// 受信
	//---------------------------------------------------------------------
	Value Capabilities()
	{
		Value sync = Value::MakeObject();
		sync.Set( "openClose", true );
		sync.Set( "change", 1 );		// 全文
		Value save = Value::MakeObject();
		save.Set( "includeText", false );
		sync.Set( "save", std::move( save ) );

		Value comp = Value::MakeObject();
		Value trig = Value::MakeArray();
		trig.Push( "." );
		comp.Set( "triggerCharacters", std::move( trig ) );

		Value sigHelp = Value::MakeObject();
		Value sigTrig = Value::MakeArray();
		sigTrig.Push( "(" );
		sigTrig.Push( "," );
		sigHelp.Set( "triggerCharacters", std::move( sigTrig ) );

		Value caps = Value::MakeObject();
		caps.Set( "textDocumentSync", std::move( sync ) );
		caps.Set( "completionProvider", std::move( comp ) );
		caps.Set( "hoverProvider", true );
		caps.Set( "definitionProvider", true );
		caps.Set( "signatureHelpProvider", std::move( sigHelp ) );
		caps.Set( "documentSymbolProvider", true );
		return caps;
	}

	void ApplySettings( const Value& s )
	{
		const Value& a = s["atomscript"];
		const Value& src = a.IsObject() ? a : s;
		if( src["manifest"].IsString() ) manifestSetting = src["manifest"].AsString();
	}

	std::vector<std::string> Handle( const std::string& text )
	{
		std::vector<std::string> out;
		Value msg;
		if( !json::Parse( text, &msg ) || !msg.IsObject() ){
			out.push_back( ErrorResponse( Value(), -32700, "parse error" ) );
			return out;
		}
		const std::string method = msg["method"].AsString();
		const Value& id = msg["id"];
		const bool isRequest = msg.Has( "id" ) && !method.empty();
		const Value& params = msg["params"];

		auto doc = [&]() -> Document* {
			auto it = docs.find( params["textDocument"]["uri"].AsString() );
			return it == docs.end() ? nullptr : &it->second;
		};

		if( method == "initialize" ){
			if( params["rootUri"].IsString() ) rootPath = UriToPath( params["rootUri"].AsString() );
			else if( params["rootPath"].IsString() ) rootPath = params["rootPath"].AsString();
			ApplySettings( params["initializationOptions"] );
			Value info = Value::MakeObject();
			info.Set( "name", "atsc" );
			info.Set( "version", "1.0" );
			Value r = Value::MakeObject();
			r.Set( "capabilities", Capabilities() );
			r.Set( "serverInfo", std::move( info ) );
			out.push_back( Response( id, std::move( r ) ) );
		} else if( method == "initialized" ){
		} else if( method == "shutdown" ){
			shutdown = true;
			out.push_back( Response( id, Value() ) );
		} else if( method == "exit" ){
			exit = true;
		} else if( method == "textDocument/didOpen" ){
			const Value& td = params["textDocument"];
			Document d;
			d.uri  = td["uri"].AsString();
			d.path = UriToPath( d.uri );
			d.text = td["text"].AsString();
			Document& ref = docs[d.uri] = std::move( d );
			out = Validate( ref );
		} else if( method == "textDocument/didChange" ){
			if( Document* d = doc() ){
				const Value& changes = params["contentChanges"];
				if( changes.Size() ) d->text = changes[changes.Size() - 1]["text"].AsString();
				out = Validate( *d );
			}
		} else if( method == "textDocument/didSave" ){
			if( Document* d = doc() ) out = Validate( *d );
		} else if( method == "textDocument/didClose" ){
			std::string uri = params["textDocument"]["uri"].AsString();
			docs.erase( uri );
			Value p = Value::MakeObject();
			p.Set( "uri", uri );
			p.Set( "diagnostics", Value::MakeArray() );
			out.push_back( Notification( "textDocument/publishDiagnostics", std::move( p ) ) );
		} else if( method == "workspace/didChangeConfiguration" ){
			ApplySettings( params["settings"] );
			out = ValidateAll();
		} else if( method == "workspace/didChangeWatchedFiles" ){
			out = ValidateAll();		// マニフェストが変わった
		} else if( method == "textDocument/completion" ){
			Document* d = doc();
			out.push_back( Response( id, d ? Completion( *d, params["position"] ) : Value::MakeArray() ) );
		} else if( method == "textDocument/hover" ){
			Document* d = doc();
			out.push_back( Response( id, d ? Hover( *d, params["position"] ) : Value() ) );
		} else if( method == "textDocument/definition" ){
			Document* d = doc();
			out.push_back( Response( id, d ? Definition( *d, params["position"] ) : Value() ) );
		} else if( method == "textDocument/signatureHelp" ){
			Document* d = doc();
			out.push_back( Response( id, d ? SignatureHelp( *d, params["position"] ) : Value() ) );
		} else if( method == "textDocument/documentSymbol" ){
			Document* d = doc();
			out.push_back( Response( id, d ? DocumentSymbols( *d ) : Value::MakeArray() ) );
		} else if( isRequest ){
			out.push_back( ErrorResponse( id, -32601, "method not found: " + method ) );
		}
		return out;
	}
};

LanguageServer::LanguageServer() : m_impl( new Impl() ) {}
LanguageServer::~LanguageServer() = default;

std::vector<std::string> LanguageServer::Handle( const std::string& message ) { return m_impl->Handle( message ); }
bool LanguageServer::ShouldExit() const { return m_impl->exit; }
bool LanguageServer::ShutdownRequested() const { return m_impl->shutdown; }

//=========================================================================
// 標準入出力（Content-Length で区切る）
//=========================================================================
int RunLanguageServer()
{
#if defined(_WIN32)
	_setmode( _fileno( stdin ), _O_BINARY );
	_setmode( _fileno( stdout ), _O_BINARY );
#endif
	LanguageServer server;
	std::string line;
	for( ;; ){
		size_t length = 0;
		bool any = false;
		// ヘッダー
		for( ;; ){
			if( !std::getline( std::cin, line ) ) return server.ShutdownRequested() ? 0 : 1;
			any = true;
			if( !line.empty() && line.back() == '\r' ) line.pop_back();
			if( line.empty() ) break;
			const char* key = "Content-Length:";
			if( line.compare( 0, std::strlen( key ), key ) == 0 ) length = (size_t)std::strtoul( line.c_str() + std::strlen( key ), nullptr, 10 );
		}
		if( !any || length == 0 ) continue;
		std::string body( length, '\0' );
		if( !std::cin.read( &body[0], (std::streamsize)length ) ) return 1;

		for( const std::string& o : server.Handle( body ) ){
			std::string header = "Content-Length: " + std::to_string( o.size() ) + "\r\n\r\n";
			std::fwrite( header.data(), 1, header.size(), stdout );
			std::fwrite( o.data(), 1, o.size(), stdout );
		}
		std::fflush( stdout );
		if( server.ShouldExit() ) return server.ShutdownRequested() ? 0 : 1;
	}
}

}	// namespace compiler
}	// namespace ats
