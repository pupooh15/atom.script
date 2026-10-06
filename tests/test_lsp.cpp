/**************************************************************************/
/*!	\file	test_lsp.cpp
	\brief	言語サーバー（atsc lsp）
***************************************************************************/
#include "test_framework.h"
#include "ats_json.h"
#include "ats_lsp.h"

#include <fstream>
#include <sstream>

using namespace ats::compiler;
using ats::json::Value;

namespace {

// samples フォルダの中に置いたことにする文書の URI（VS Code と同じく ':' を %3A にする）
std::string DocUri( const std::string& name )
{
	std::string p = ATS_SAMPLES_DIR;
	std::string uri = "file:///";
	for( char c : p ){
		if( c == ':' ) uri += "%3A";
		else if( c == '\\' ) uri += '/';
		else uri += c;
	}
	return uri + "/" + name;
}

struct Client {
	LanguageServer	server;
	int				nextId = 1;

	std::vector<Value> Send( const std::string& method, Value params, bool request )
	{
		Value msg = Value::MakeObject();
		msg.Set( "jsonrpc", "2.0" );
		if( request ) msg.Set( "id", nextId++ );
		msg.Set( "method", method );
		msg.Set( "params", std::move( params ) );
		std::vector<Value> out;
		for( const std::string& s : server.Handle( msg.Dump() ) ){
			Value v;
			if( ats::json::Parse( s, &v ) ) out.push_back( v );
		}
		return out;
	}

	Value Request( const std::string& method, Value params )
	{
		std::vector<Value> r = Send( method, std::move( params ), true );
		return r.empty() ? Value() : r[0];
	}

	void Initialize( bool withManifest = true )
	{
		Value p = Value::MakeObject();
		Value opt = Value::MakeObject();
		if( withManifest ) opt.Set( "manifest", std::string( ATS_SAMPLES_DIR ) + "/sample.atsmanifest.yaml" );
		p.Set( "initializationOptions", std::move( opt ) );
		Request( "initialize", std::move( p ) );
		Send( "initialized", Value::MakeObject(), false );
	}

	// 開いて、その文書の診断を返す
	Value Open( const std::string& uri, const std::string& text )
	{
		Value td = Value::MakeObject();
		td.Set( "uri", uri );
		td.Set( "languageId", "atomscript" );
		td.Set( "version", 1 );
		td.Set( "text", text );
		Value p = Value::MakeObject();
		p.Set( "textDocument", std::move( td ) );
		for( const Value& n : Send( "textDocument/didOpen", std::move( p ), false ) )
			if( n["params"]["uri"].AsString() == uri ) return n["params"]["diagnostics"];
		return Value();
	}

	static Value At( const std::string& uri, int line, int ch )
	{
		Value td = Value::MakeObject();
		td.Set( "uri", uri );
		Value pos = Value::MakeObject();
		pos.Set( "line", line );
		pos.Set( "character", ch );
		Value p = Value::MakeObject();
		p.Set( "textDocument", std::move( td ) );
		p.Set( "position", std::move( pos ) );
		return p;
	}
};

const Value* FindItem( const Value& items, const std::string& label )
{
	for( size_t i = 0; i < items.Size(); ++i ) if( items[i]["label"].AsString() == label ) return &items[i];
	return nullptr;
}

const char* kScript =
	"script \"sample/lsp\"\n"							// 0
	"var talked: int = 0\n"								// 1
	"event OnTalk(target: handle) {\n"					// 2
	"    let answer = 1\n"								// 3
	"    await ShowMessage(\"hi\", face: Face.Smile)\n"	// 4
	"    \n"											// 5
	"}\n"												// 6
	"fn GiveReward(n: int) -> int { return n }\n";		// 7

}	// namespace

TEST( LspInitializeAndDiagnostics )
{
	Client c;
	Value p = Value::MakeObject();
	Value init = c.Request( "initialize", std::move( p ) );
	CHECK( init["result"]["capabilities"]["hoverProvider"].AsBool() );
	CHECK( init["result"]["capabilities"]["completionProvider"].IsObject() );

	// マニフェストは文書のフォルダから見つける（samples/sample.atsmanifest.yaml）
	std::string uri = DocUri( "diag.ats" );
	Value diags = c.Open( uri, "script \"s/d\"\nevent OnTalk(target: handle) {\n    ShowMessage(\"x\")\n}\n" );
	REQUIRE( diags.Size() == 1 );
	CHECK( diags[0]["message"].AsString().find( "await が必要" ) != std::string::npos );
	CHECK_EQ( diags[0]["range"]["start"]["line"].AsInt(), 2 );
	CHECK_EQ( diags[0]["range"]["start"]["character"].AsInt(), 4 );
	CHECK_EQ( diags[0]["range"]["end"]["character"].AsInt(), 15 );		// "ShowMessage" の終わり
	CHECK_EQ( diags[0]["severity"].AsInt(), 1 );

	// 直すと診断が消える
	Value td = Value::MakeObject();
	td.Set( "uri", uri );
	Value change = Value::MakeObject();
	change.Set( "text", "script \"s/d\"\nevent OnTalk(target: handle) {\n    await ShowMessage(\"x\")\n}\n" );
	Value changes = Value::MakeArray();
	changes.Push( change );
	Value cp = Value::MakeObject();
	cp.Set( "textDocument", td );
	cp.Set( "contentChanges", changes );
	std::vector<Value> out = c.Send( "textDocument/didChange", std::move( cp ), false );
	REQUIRE( !out.empty() );
	CHECK_EQ( out[0]["params"]["diagnostics"].Size(), (size_t)0 );
}

TEST( LspMissingManifest )
{
	Client c;
	c.Initialize( false );
	Value diags = c.Open( "file:///C%3A/nowhere/at/all/x.ats", "script \"s/x\"\nevent OnStart() {}\n" );
	REQUIRE( diags.Size() == 1 );
	CHECK_EQ( diags[0]["severity"].AsInt(), 2 );
	CHECK( diags[0]["message"].AsString().find( "マニフェスト" ) != std::string::npos );
}

TEST( LspCompletion )
{
	Client c;
	c.Initialize();
	std::string uri = DocUri( "comp.ats" );
	c.Open( uri, kScript );

	// enum の値
	Value r = c.Request( "textDocument/completion", Client::At( uri, 4, 40 ) );	// "Face.Smi|le"
	CHECK( FindItem( r["result"], "Smile" ) != nullptr );
	CHECK( FindItem( r["result"], "Normal" ) != nullptr );
	CHECK( FindItem( r["result"], "ShowMessage" ) == nullptr );

	// 空行：コマンド・キーワード・var・引数・let
	r = c.Request( "textDocument/completion", Client::At( uri, 5, 4 ) );
	const Value* show = FindItem( r["result"], "ShowMessage" );
	REQUIRE( show );
	CHECK_EQ( (*show)["insertText"].AsString(), std::string( "await ShowMessage(${1:text})" ) );	CHECK( (*show)["filterText"].AsString().find( "メッセージ表示" ) != std::string::npos );
	CHECK( (*show)["detail"].AsString().find( "face: Face = Normal" ) != std::string::npos );
	const Value* shake = FindItem( r["result"], "CameraShake" );
	REQUIRE( shake );
	CHECK_EQ( (*shake)["insertText"].AsString(), std::string( "CameraShake()" ) );		// 既定値のある引数は雛形に入れない
	CHECK( FindItem( r["result"], "talked" ) != nullptr );
	CHECK( FindItem( r["result"], "target" ) != nullptr );
	CHECK( FindItem( r["result"], "answer" ) != nullptr );
	CHECK( FindItem( r["result"], "GiveReward" ) != nullptr );
	CHECK( FindItem( r["result"], "parallel" ) != nullptr );
	CHECK( FindItem( r["result"], "story" ) != nullptr );
	CHECK( FindItem( r["result"], "n" ) == nullptr );				// 別の関数の引数は出さない

	// バンクの変数
	Value td = Value::MakeObject();
	c.Open( DocUri( "bank.ats" ), "script \"s/b\"\nevent OnStart() {\n    story.\n}\n" );
	r = c.Request( "textDocument/completion", Client::At( DocUri( "bank.ats" ), 2, 10 ) );
	CHECK( FindItem( r["result"], "chapter" ) != nullptr );
	CHECK( FindItem( r["result"], "met_merchant" ) != nullptr );
}

TEST( LspHoverDefinitionSignatureSymbols )
{
	Client c;
	c.Initialize();
	std::string uri = DocUri( "nav.ats" );
	c.Open( uri, kScript );

	// ホバー
	Value h = c.Request( "textDocument/hover", Client::At( uri, 4, 14 ) );		// ShowMessage
	std::string md = h["result"]["contents"]["value"].AsString();
	CHECK( md.find( "await ShowMessage(text: string, face: Face = Normal)" ) != std::string::npos );
	CHECK( md.find( "メッセージ表示" ) != std::string::npos );
	h = c.Request( "textDocument/hover", Client::At( uri, 4, 40 ) );			// Face.Smile
	CHECK( h["result"]["contents"]["value"].AsString().find( "Face.Smile = 1" ) != std::string::npos );
	h = c.Request( "textDocument/hover", Client::At( uri, 1, 6 ) );				// talked
	CHECK( h["result"]["contents"]["value"].AsString().find( "var talked: int" ) != std::string::npos );

	// 定義へ移動：コマンドはマニフェストの該当行へ
	Value d = c.Request( "textDocument/definition", Client::At( uri, 4, 14 ) );
	CHECK( d["result"]["uri"].AsString().find( "sample.atsmanifest.yaml" ) != std::string::npos );
	{
		std::ifstream f( std::string( ATS_SAMPLES_DIR ) + "/sample.atsmanifest.yaml" );
		std::string line;
		int n = 0, want = -1;
		while( std::getline( f, line ) ){ if( line.find( "name: ShowMessage" ) != std::string::npos ) want = n; ++n; }
		CHECK_EQ( d["result"]["range"]["start"]["line"].AsInt(), want );
	}
	// 関数は同じファイルへ
	c.Open( DocUri( "nav2.ats" ), std::string( kScript ) + "event OnX() { let v = GiveReward(1) }\n" );
	d = c.Request( "textDocument/definition", Client::At( DocUri( "nav2.ats" ), 8, 25 ) );
	CHECK_EQ( d["result"]["range"]["start"]["line"].AsInt(), 7 );

	// 引数ヒント
	c.Open( DocUri( "sig.ats" ), "script \"s/s\"\nevent OnStart() {\n    await ShowMessage(\"a\", \n}\n" );
	Value s = c.Request( "textDocument/signatureHelp", Client::At( DocUri( "sig.ats" ), 2, 27 ) );
	CHECK_EQ( s["result"]["activeParameter"].AsInt(), 1 );
	CHECK_EQ( s["result"]["signatures"][0]["label"].AsString(), std::string( "ShowMessage(text: string, face: Face = Normal)" ) );

	// アウトライン
	Value sym = c.Request( "textDocument/documentSymbol", Value( Client::At( uri, 0, 0 ) ) );
	std::string names;
	for( size_t i = 0; i < sym["result"].Size(); ++i ) names += sym["result"][i]["name"].AsString() + ",";
	CHECK_EQ( names, std::string( "talked,OnTalk,GiveReward," ) );
}

TEST( LspProtocol )
{
	Client c;
	c.Initialize();
	Value r = c.Request( "textDocument/unknownThing", Value::MakeObject() );
	CHECK_EQ( r["error"]["code"].AsInt(), -32601 );
	CHECK( c.server.Handle( "{ not json" ).size() == 1 );
	r = c.Request( "shutdown", Value() );
	CHECK( r.Has( "result" ) );
	c.Send( "exit", Value(), false );
	CHECK( c.server.ShouldExit() );
	CHECK( c.server.ShutdownRequested() );
}

TEST( JsonRoundTrip )
{
	Value v;
	REQUIRE( ats::json::Parse( "{\"a\":[1,2.5,\"x\\u3042\\n\",true,null],\"b\":{\"c\":-3}}", &v ) );
	CHECK_EQ( v["a"][1].AsNumber(), 2.5 );
	CHECK_EQ( v["a"][2].AsString(), std::string( "xあ\n" ) );
	CHECK_EQ( v["b"]["c"].AsInt(), -3 );
	CHECK_EQ( v.Dump(), std::string( "{\"a\":[1,2.5,\"xあ\\n\",true,null],\"b\":{\"c\":-3}}" ) );
	CHECK( !ats::json::Parse( "[1,", &v ) );
	CHECK( !ats::json::Parse( "{} x", &v ) );
}
