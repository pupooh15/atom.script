/**************************************************************************/
/*!	\file	test_compiler.cpp
	\brief	コンパイラ（.ats → .atsb → VM で実行）とエラー報告
***************************************************************************/
#include "test_framework.h"
#include "ats_compiler.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace ats::compiler;

namespace {

const char* kManifest = R"(
manifest: 1
project: test

enums:
  Face: { values: { Normal: 0, Smile: 1, Angry: 2 } }
  Rank:
    values: { Start: 0, Lab: 1, Warehouse: 2 }

variable_banks:
  story:
    scope: persistent
    vars:
      - { id: 1001, name: chapter, type: int, init: 1 }
      - { id: 1002, name: hero,    type: string, init: "Red" }
  scene:
    scope: session
    vars:
      - { id: 2001, name: count, type: int }

events:
  - name: OnTalk
    params: [ { name: target, type: handle } ]

commands:
  - { name: Report,  params: [ { name: v, type: int } ] }
  - { name: ReportF, params: [ { name: v, type: float } ] }
  - { name: ReportS, params: [ { name: v, type: string } ] }
  - { name: ReportB, params: [ { name: v, type: bool } ] }
  - name: Talk
    latent: true
    channel: message
    params:
      - { name: text, type: string }
      - { name: face, type: Face, default: Normal }
  - name: Ask
    latent: true
    returns: int
  - name: OldCommand
    deprecated: true

queries:
  - name: Double
    returns: int
    params: [ { name: x, type: int } ]
)";

// テスト用のホスト。マニフェストから生成される登録コードの代わり
struct Host {
	Env*				env;
	std::vector<int>	answers;	// Ask が返す値（先頭から）
};

ats_status ATS_CALL TalkFn( ats_call* call, void* user )
{
	Host* h = static_cast<Host*>( user );
	h->env->reports.push_back( std::string( "Talk:" ) + ats_arg_string( call, 0 ) + ":" + std::to_string( ats_arg_int( call, 1 ) ) );
	return ATS_DONE;
}

ats_status ATS_CALL AskFn( ats_call* call, void* user )
{
	Host* h = static_cast<Host*>( user );
	int v = 0;
	if( !h->answers.empty() ){ v = h->answers.front(); h->answers.erase( h->answers.begin() ); }
	ats_value r = V_Int( v );
	ats_call_set_result( call, &r );
	return ATS_DONE;
}

ats_status ATS_CALL NopFn( ats_call*, void* ) { return ATS_DONE; }

ats_result ATS_CALL DoubleFn( ats_call* call, void* )
{
	ats_value r = V_Int( ats_arg_int( call, 0 ) * 2 );
	return ats_call_set_result( call, &r );
}

void Setup( Env& env, Host& host, const Manifest& m )
{
	host.env = &env;
	env.RegisterReporters();
	auto reg = [&]( const char* name, ats_command_fn fn ){
		const MCommand* c = m.FindCommand( name );
		ats_command_desc d{};
		d.size     = sizeof(d);
		d.name     = name;
		d.sig_hash = c ? c->SignatureHash() : 0;
		d.channel  = c && !c->channel.empty() ? c->channel.c_str() : nullptr;
		d.fn       = fn;
		d.user     = &host;
		ats_register_command( env.rt, &d );
	};
	reg( "Talk", TalkFn );
	reg( "Ask", AskFn );
	reg( "OldCommand", NopFn );
	ats_query_desc q{};
	q.size     = sizeof(q);
	q.name     = "Double";
	q.sig_hash = m.FindCommand( "Double" )->SignatureHash();
	q.fn       = DoubleFn;
	ats_register_query( env.rt, &q );

	// 共有変数（生成コードが ats_define_var で登録する想定）
	for( const MVar& v : m.vars ){
		ats_var_desc d{};
		d.size  = sizeof(d);
		d.id    = v.id;
		d.name  = v.name.c_str();
		d.type  = v.type.base;
		d.scope = v.scope;
		if( v.init.present ){
			if( v.type.base == ATS_TYPE_STRING ) d.init = V_Str( v.init.text.c_str() );
			else d.init = V_Int( std::atoi( v.init.text.c_str() ) );
		}
		ats_define_var( env.rt, &d );
	}
	env.MakeVm();
}

Manifest LoadManifest()
{
	Manifest m;
	Diagnostics d;
	m.LoadText( kManifest, "test.atsmanifest.yaml", d );
	if( d.HasErrors() ) std::printf( "%s", d.ToText().c_str() );
	return m;
}

// コンパイルして読み込む。失敗したら診断を表示して nullptr
ats_program* Build( Env& env, const Manifest& m, const std::string& src, Diagnostics* outDiag = nullptr )
{
	Diagnostics d;
	CompileResult r;
	bool ok = CompileSource( src, "test.ats", m, d, &r );
	if( outDiag ) *outDiag = d;
	if( !ok ){
		std::printf( "%s", d.ToText().c_str() );
		return nullptr;
	}
	ats_program* p = nullptr;
	if( ats_program_load( env.rt, r.bytes.data(), r.bytes.size(), &p ) != ATS_OK )
		std::printf( "  load error: %s\n", ats_runtime_last_error( env.rt ) );
	return p;
}

// コンパイルエラーになることを確かめる（メッセージの一部と行）
bool ExpectError( const Manifest& m, const std::string& src, const std::string& msg, int line )
{
	Diagnostics d;
	CompileResult r;
	if( CompileSource( src, "test.ats", m, d, &r ) ){
		std::printf( "  expected error '%s' but compiled\n", msg.c_str() );
		return false;
	}
	for( const Diagnostic& x : d.List() )
		if( x.severity == Diagnostic::Error && x.message.find( msg ) != std::string::npos && (line == 0 || x.line == line) ) return true;
	std::printf( "  expected error '%s' at line %d, got:\n%s", msg.c_str(), line, d.ToText().c_str() );
	return false;
}

}	// namespace

//=========================================================================
// 実行して確かめる
//=========================================================================
TEST( CompileExpressionsAndControlFlow )
{
	Env env; Host host;
	Manifest m = LoadManifest();
	Setup( env, host, m );

	ats_program* p = Build( env, m, R"(
script "test/expr"

fn Fact(n: int) -> int {
    if (n <= 1) { return 1 }
    return n * Fact(n - 1)
}

fn Sign(x: int) -> int {
    if (x < 0) { return -1 } else if (x == 0) { return 0 } else { return 1 }
}

event OnStart() {
    Report(1 + 2 * 3)                  // 7
    Report((1 + 2) * 3)                // 9
    Report(-7 / 2)                     // -3
    Report(17 % 5)                     // 2
    Report(1 << 4 | 1)                 // 17
    ReportF(1 + 0.5)                   // 1.5（int は float に昇格）
    Report(int(2.9))                   // 2
    ReportF(float(3) / 2)              // 1.5
    Report(Fact(5))                    // 120
    Report(Sign(-4) + Sign(0) + Sign(9))   // 0
    Report(min(3, 8) + max(3, 8))      // 11
    Report(abs(-5))                    // 5
    ReportB(1 < 2 && 2 < 3)            // true
    ReportB(1 > 2 || !(2 > 3))         // true
    ReportB("abc" == "abc")            // true
    Report(Double(21))                 // 42

    let total = 0
    loop (4) { total += 10 }
    Report(total)                      // 40

    let i = 0
    loop {
        i += 1
        if (i == 2) { continue }
        if (i > 4) { break }
        Report(100 + i)                // 101, 103, 104
    }
}
)" );
	REQUIRE( p );
	env.Fire( p, "OnStart" );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "7,9,-3,2,17,1.5,2,1.5,120,0,11,5,true,true,true,42,40,101,103,104" ) );
	CHECK( env.errors.empty() );
	ats_program_release( p );
}

TEST( CompileSwitchEnumsAndVars )
{
	Env env; Host host;
	Manifest m = LoadManifest();
	Setup( env, host, m );

	ats_program* p = Build( env, m, R"(
script "test/vars"

var rank: Rank = Rank.Start
var label: string = "start"
transient var temp: int = 5

event OnStep() {
    switch (rank) {
    case Rank.Start     { Report(0)  rank = Rank.Lab }
    case Rank.Lab       { Report(1)  rank = Rank.Warehouse }
    default             { Report(99) }
    }
}

event OnShared() {
    Report(story.chapter)              // 1（マニフェストの初期値）
    story.chapter += 2
    Report(story.chapter)              // 3
    ReportS(story.hero)                // Red
    label = "lab"
    ReportS(label)
    Report(temp)
}

event OnReset() {
    reset vars
    Report(int(rank))
}
)" );
	REQUIRE( p );
	env.Fire( p, "OnStep" ); env.Update();
	env.Fire( p, "OnStep" ); env.Update();
	env.Fire( p, "OnStep" ); env.Update();
	env.Fire( p, "OnShared" ); env.Update();
	env.Fire( p, "OnReset" ); env.Update();
	CHECK_EQ( env.Reports(), std::string( "0,1,99,1,3,Red,lab,5,0" ) );
	ats_program_release( p );
}

TEST( CompileAwaitParallelAndRace )
{
	Env env; Host host;
	Manifest m = LoadManifest();
	Setup( env, host, m );
	host.answers = { 1, 0 };

	ats_program* p = Build( env, m, R"(
script "test/await"

fn Confirm() -> bool {
    let a = await Ask()
    return a == 0
}

event OnTalk(target: handle) {
    await Talk("hello", face: Face.Smile)
    loop {
        if (await Confirm()) { break }
        await Talk("again?")
    }
    parallel {
        branch { wait 0.5  Report(1) }
        branch { wait 0.2  Report(2) }
    }
    race {
        branch { wait 10  Report(3) }
        branch { wait frames 2  Report(4) }
    }
    Report(5)
}
)" );
	REQUIRE( p );
	ats_value h = V_Handle( 7 );
	CHECK_EQ( ats_vm_fire_event( env.vm, p, "OnTalk", &h, 1, nullptr ), ATS_OK );
	for( int i = 0; i < 10; ++i ) env.Update( 0.1f );
	CHECK_EQ( env.Reports(), std::string( "Talk:hello:1,Talk:again?:0,2,1,4,5" ) );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	ats_program_release( p );
}

TEST( CompileFireBetweenScripts )
{
	Env env; Host host;
	Manifest m = LoadManifest();
	Setup( env, host, m );

	ats_program* a = Build( env, m, R"(
script "test/a"
event OnStart(who: handle) {
    fire OnTalk(target: who)
    fire OnSignal(5)
}
)" );
	ats_program* b = Build( env, m, R"(
script "test/b"
event OnTalk(target: handle) { Report(1) }
event OnSignal(v: int) { Report(v * 10) }
)" );
	REQUIRE( a && b );
	ats_vm_attach( env.vm, b );
	env.Fire( a, "OnStart", { V_Handle( 3 ) } );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "1,50" ) );
	ats_program_release( a );
	ats_program_release( b );
}

TEST( CompileSampleFiles )
{
	Env env; Host host;
	Manifest m;
	Diagnostics d;
	std::string dir = ATS_SAMPLES_DIR;
	REQUIRE( m.Load( dir + "/sample.atsmanifest.yaml", d ) );

	std::ifstream f( dir + "/merchant.ats", std::ios::binary );
	std::stringstream ss;
	ss << f.rdbuf();
	CompileResult r;
	bool ok = CompileSource( ss.str(), "merchant.ats", m, d, &r );
	if( !ok ) std::printf( "%s", d.ToText().c_str() );
	CHECK( ok );
	CHECK_EQ( r.scriptId, std::string( "sample/merchant" ) );

	std::string text, err;
	CHECK( Disassemble( r.bytes, &text, &err ) );
	CHECK( text.find( "OnTalk:" ) != std::string::npos );
	CHECK( text.find( "FORK" ) != std::string::npos );
	(void)env; (void)host;
}

TEST( CompilerSurvivesBrokenInput )
{
	// サンプルを途中で切ったもの・1 文字壊したものを全部コンパイルしても落ちない
	Manifest m;
	Diagnostics md;
	std::string dir = ATS_SAMPLES_DIR;
	REQUIRE( m.Load( dir + "/sample.atsmanifest.yaml", md ) );
	std::ifstream f( dir + "/merchant.ats", std::ios::binary );
	std::stringstream ss;
	ss << f.rdbuf();
	const std::string src = ss.str();
	const char junk[] = { '{', '}', '(', ')', '"', '@', 'x', '\n', '=', '.' };
	int compiled = 0;
	for( size_t i = 0; i < src.size(); ++i ){
		Diagnostics d;
		CompileResult r;
		if( CompileSource( src.substr( 0, i ), "cut.ats", m, d, &r ) ) ++compiled;
		std::string bad = src;
		bad[i] = junk[i % sizeof(junk)];
		Diagnostics d2;
		CompileSource( bad, "bad.ats", m, d2, &r );
	}
	CHECK( compiled > 0 );	// 途中までで正しいプログラムになる切り方もある
}

//=========================================================================
// エラー報告
//=========================================================================
TEST( CompileErrors )
{
	Manifest m = LoadManifest();
	const std::string h = "script \"t/e\"\n";

	CHECK( ExpectError( m, "event OnStart() {}\n", "script", 1 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  Talk(\"x\")\n}\n", "await が必要", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  await Report(1)\n}\n", "await は不要", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  Report(nope)\n}\n", "'nope' は宣言されていません", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  let x: int = \"s\"\n}\n", "型が合いません", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  loop { Report(1) }\n}\n", "無限ループ", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  let n = 0\n  parallel { branch { n = 1 } }\n}\n", "外側の let 変数", 4 ) );
	CHECK( ExpectError( m, h + "fn F() -> int {\n  Report(1)\n}\n", "return がありません", 4 ) );
	CHECK( ExpectError( m, h + "event OnTalk(x: int) {}\n", "マニフェストと違います", 2 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  break\n}\n", "loop の中でだけ", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  switch (1) { case 1 {} case 1 {} }\n}\n", "重複", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  Report(1\n}\n", "')' が必要", 4 ) );
	CHECK( ExpectError( m, h + "fn W() { wait 1 }\nevent OnStart() {\n  W()\n}\n", "関数 'W' は待機を含む", 4 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  Report(Face.Sad)\n}\n", "値 'Sad' はありません", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  Report(story.nope)\n}\n", "変数 'nope' はありません", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  Report(1, 2)\n}\n", "引数が多すぎます", 3 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  await Talk(face: Face.Smile)\n}\n", "引数 'text' がありません", 3 ) );
	CHECK( ExpectError( m, h + "var a: int = 1 + 2\n", "定数", 2 ) );
	CHECK( ExpectError( m, h + "event OnStart() {\n  Report(1) == 2\n}\n", "", 3 ) );
}

TEST( CompileWarnings )
{
	Manifest m = LoadManifest();
	Diagnostics d;
	CompileResult r;
	bool ok = CompileSource( "script \"t/w\"\nevent OnStart() {\n  return\n  Report(1)\n  OldCommand()\n  fire Unknown(1)\n}\n",
							 "test.ats", m, d, &r );
	CHECK( ok );
	int warnings = 0;
	for( const Diagnostic& x : d.List() ) if( x.severity == Diagnostic::Warning ) ++warnings;
	CHECK_EQ( warnings, 3 );	// 到達しない・非推奨・未知のイベント
	CHECK( d.ToJson().find( "\"severity\":\"warning\"" ) != std::string::npos );
}

TEST( ManifestErrors )
{
	auto expect = []( const char* text, const char* msg ){
		Manifest m;
		Diagnostics d;
		bool ok = m.LoadText( text, "m.yaml", d );
		bool found = d.ToText().find( msg ) != std::string::npos;
		if( ok || !found ) std::printf( "  manifest: expected '%s', got:\n%s", msg, d.ToText().c_str() );
		return !ok && found;
	};
	CHECK( expect( "manifest: 1\nvariable_banks:\n  a:\n    vars:\n      - { id: 1, name: x, type: int }\n      - { id: 1, name: y, type: int }\n", "重複" ) );
	CHECK( expect( "manifest: 1\ncommands:\n  - { name: A, params: [ { name: p, type: Vec3 } ] }\n", "未知の型 'Vec3'" ) );
	CHECK( expect( "manifest: 1\ncommands:\n  - name: A\n    latent: maybe\n", "latent" ) );
	CHECK( expect( "manifest: 2\n", "manifest: 1" ) );
	CHECK( expect( "manifest: 1\nenums:\n  E: { values: { A: 0, A: 1 } }\n", "重複" ) );
	CHECK( expect( "manifest: 1\n  bad indent: 1\n", "インデント" ) );
}
