/**************************************************************************/
/*!	\file	test_vars.cpp
	\brief	変数（スクリプト変数・共有変数・セーブ／ロード・名前変更・型変更）
***************************************************************************/
#include "test_framework.h"

using namespace ats::fmt;
using ats::writer::ProgramWriter;

namespace {

constexpr uint32_t kChapter = 1001;		// story.chapter（persistent）
constexpr uint32_t kName    = 1002;		// story.name（persistent, string）
constexpr uint32_t kDoor    = 2001;		// scene.door_opened（session）

void DefineShared( Env& env )
{
	ats_var_desc d{};
	d.size  = sizeof(d);

	d.id    = kChapter;
	d.name  = "story.chapter";
	d.type  = ATS_TYPE_INT;
	d.scope = ATS_SCOPE_PERSISTENT;
	d.init  = V_Int( 1 );
	ats_define_var( env.rt, &d );

	d.id    = kName;
	d.name  = "story.name";
	d.type  = ATS_TYPE_STRING;
	d.init  = V_Str( "none" );
	ats_define_var( env.rt, &d );

	d.id    = kDoor;
	d.name  = "scene.door_opened";
	d.type  = ATS_TYPE_BOOL;
	d.scope = ATS_SCOPE_SESSION;
	d.init  = V_Bool( false );
	ats_define_var( env.rt, &d );
}

// el_rescue 相当のスクリプト。OnSet で値を書き、OnReport で全部を報告する
ats_program* LoadQuest( Env& env, const char* retryName = "retry", const char* was = "" )
{
	ProgramWriter w( "golden_lore/el_rescue" );
	uint16_t rep  = w.Command( "Report", { ATS_TYPE_INT } );
	uint16_t repb = w.Command( "ReportB", { ATS_TYPE_BOOL } );
	uint16_t reps = w.Command( "ReportS", { ATS_TYPE_STRING } );
	uint16_t chapter = w.SharedVar( kChapter, ATS_TYPE_INT );
	uint16_t name    = w.SharedVar( kName, ATS_TYPE_STRING );
	uint16_t door    = w.SharedVar( kDoor, ATS_TYPE_BOOL );
	uint16_t retry   = w.ScriptVar( retryName, ATS_TYPE_INT, 0, false, was );
	uint16_t temp    = w.ScriptVar( "temp", ATS_TYPE_INT, 0, true );
	uint16_t label   = w.ScriptVarStr( "label", "start" );

	w.BeginEvent( "OnSet", {}, 0 );
	w.PushI( 3 );          w.StVar( chapter );
	w.PushStr( "castle" ); w.StVar( name );
	w.PushI( 1 );          w.StVar( door );
	w.PushI( 2 );          w.StVar( retry );
	w.PushI( 5 );          w.StVar( temp );
	w.PushStr( "lab" );    w.StVar( label );
	w.Op( OP_END );
	w.EndEntry();

	w.BeginEvent( "OnReport", {}, 0 );
	w.LdVar( chapter ); w.CallCmd( rep );
	w.LdVar( name );    w.CallCmd( reps );
	w.LdVar( door );    w.CallCmd( repb );
	w.LdVar( retry );   w.CallCmd( rep );
	w.LdVar( temp );    w.CallCmd( rep );
	w.LdVar( label );   w.CallCmd( reps );
	w.Op( OP_END );
	w.EndEntry();

	w.BeginEvent( "OnRetry", {}, 0 );
	w.LdVar( retry ); w.PushI( 1 ); w.Op( OP_ADD_I ); w.StVar( retry );
	w.Op( OP_END );
	w.EndEntry();

	w.BeginEvent( "OnReset", {}, 0 );
	w.Op( OP_RESET_VARS );
	w.Op( OP_END );
	w.EndEntry();
	return env.Load( w );
}

std::string RunReport( Env& env, ats_program* prog )
{
	env.reports.clear();
	env.Fire( prog, "OnReport" );
	env.Update();
	return env.Reports();
}

MemoryFile Save( Env& env )
{
	MemoryFile f;
	ats_vm_save( env.vm, MemoryFile::Write, &f );
	return f;
}

}	// namespace

//=========================================================================
// スクリプト変数
//=========================================================================
TEST( ScriptVarsAreSharedAcrossEvents )
{
	Env env;
	env.RegisterReporters();
	DefineShared( env );
	env.MakeVm();
	ats_program* prog = LoadQuest( env );
	REQUIRE( prog );

	CHECK_EQ( RunReport( env, prog ), std::string( "1,none,false,0,0,start" ) );
	env.Fire( prog, "OnRetry" );
	env.Fire( prog, "OnRetry" );
	env.Update();
	CHECK_EQ( RunReport( env, prog ), std::string( "1,none,false,2,0,start" ) );
	ats_program_release( prog );
}

TEST( ResetVars )
{
	Env env;
	env.RegisterReporters();
	DefineShared( env );
	env.MakeVm();
	ats_program* prog = LoadQuest( env );
	REQUIRE( prog );

	env.Fire( prog, "OnSet" );
	env.Update();
	env.Fire( prog, "OnReset" );		// スクリプト変数だけが戻る
	env.Update();
	CHECK_EQ( RunReport( env, prog ), std::string( "3,castle,true,0,0,start" ) );

	env.Fire( prog, "OnSet" );
	env.Update();
	CHECK_EQ( ats_script_reset( env.vm, "golden_lore/el_rescue" ), ATS_OK );
	CHECK_EQ( RunReport( env, prog ), std::string( "3,castle,true,0,0,start" ) );
	ats_program_release( prog );
}

//=========================================================================
// 共有変数
//=========================================================================
TEST( SharedVarsFromHost )
{
	Env env;
	DefineShared( env );
	env.MakeVm();

	ats_value v;
	CHECK_EQ( ats_var_get( env.vm, kChapter, &v ), ATS_OK );
	CHECK_EQ( v.v.i, 1 );
	CHECK_EQ( ats_var_get( env.vm, kName, &v ), ATS_OK );
	CHECK_EQ( std::string( v.v.s ), std::string( "none" ) );

	ats_value nv = V_Int( 9 );
	CHECK_EQ( ats_var_set( env.vm, kChapter, &nv ), ATS_OK );
	ats_value bad = V_Str( "x" );
	CHECK_EQ( ats_var_set( env.vm, kChapter, &bad ), ATS_ERR_TYPE );
	CHECK_EQ( ats_var_get( env.vm, 9999, &v ), ATS_ERR_NOT_FOUND );
	ats_var_get( env.vm, kChapter, &v );
	CHECK_EQ( v.v.i, 9 );
}

//=========================================================================
// セーブ／ロード
//=========================================================================
TEST( SaveLoadRoundTrip )
{
	Env env;
	env.RegisterReporters();
	DefineShared( env );
	env.MakeVm();
	ats_program* prog = LoadQuest( env );
	REQUIRE( prog );
	env.Fire( prog, "OnSet" );
	env.Update();
	CHECK_EQ( RunReport( env, prog ), std::string( "3,castle,true,2,5,lab" ) );
	MemoryFile f = Save( env );

	// 新しい VM に読み込む。session の共有変数と transient のスクリプト変数は初期値に戻る
	env.MakeVm();
	CHECK_EQ( ats_vm_attach( env.vm, prog ), ATS_OK );
	CHECK_EQ( ats_vm_load( env.vm, MemoryFile::Read, &f ), ATS_OK );
	CHECK_EQ( RunReport( env, prog ), std::string( "3,castle,false,2,0,lab" ) );
	CHECK( env.warnings.empty() );
	ats_program_release( prog );
}

TEST( LoadBeforeAttachKeepsDormantValues )
{
	Env env;
	env.RegisterReporters();
	DefineShared( env );
	env.MakeVm();
	ats_program* prog = LoadQuest( env );
	REQUIRE( prog );
	env.Fire( prog, "OnSet" );
	env.Update();
	MemoryFile f1 = Save( env );

	// プログラムを読み込まないままロードしてセーブし直しても、値は失われない
	env.MakeVm();
	CHECK_EQ( ats_vm_load( env.vm, MemoryFile::Read, &f1 ), ATS_OK );
	MemoryFile f2 = Save( env );

	env.MakeVm();
	CHECK_EQ( ats_vm_load( env.vm, MemoryFile::Read, &f2 ), ATS_OK );
	CHECK_EQ( RunReport( env, prog ), std::string( "3,castle,false,2,0,lab" ) );
	ats_program_release( prog );
}

TEST( RenamedVariableWithWas )
{
	Env env;
	env.RegisterReporters();
	DefineShared( env );
	env.MakeVm();
	ats_program* v1 = LoadQuest( env, "retry_count" );
	REQUIRE( v1 );
	env.Fire( v1, "OnSet" );
	env.Update();
	MemoryFile f = Save( env );
	ats_program_release( v1 );

	// v2 では retry に改名（@was("retry_count")）
	ats_program* v2 = LoadQuest( env, "retry", "retry_count" );
	REQUIRE( v2 );

	// 結び付けてからロード
	env.MakeVm();
	ats_vm_attach( env.vm, v2 );
	ats_vm_load( env.vm, MemoryFile::Read, &f );
	CHECK_EQ( RunReport( env, v2 ), std::string( "3,castle,false,2,0,lab" ) );

	// ロードしてから結び付け（休眠データを旧名で探す）
	env.MakeVm();
	f.pos = 0;
	ats_vm_load( env.vm, MemoryFile::Read, &f );
	CHECK_EQ( RunReport( env, v2 ), std::string( "3,castle,false,2,0,lab" ) );
	ats_program_release( v2 );
}

TEST( TypeChangedVariable )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter a( "test/types" );
	uint16_t n = a.ScriptVar( "power", ATS_TYPE_INT );
	uint16_t s = a.ScriptVarStr( "mode", "" );
	a.BeginEvent( "OnSet", {}, 0 );
	a.PushI( 5 ); a.StVar( n );
	a.PushStr( "fast" ); a.StVar( s );
	a.Op( OP_END );
	a.EndEntry();
	ats_program* pa = env.Load( a );
	REQUIRE( pa );
	env.Fire( pa, "OnSet" );
	env.Update();
	MemoryFile f = Save( env );
	ats_program_release( pa );

	// power は float に、mode は int に変わった
	ProgramWriter b( "test/types" );
	uint16_t repf = b.Command( "ReportF", { ATS_TYPE_FLOAT } );
	uint16_t rep  = b.Command( "Report", { ATS_TYPE_INT } );
	uint16_t n2 = b.ScriptVar( "power", ATS_TYPE_FLOAT );
	uint16_t s2 = b.ScriptVar( "mode", ATS_TYPE_INT, (uint64_t)(uint32_t)7 );
	b.BeginEvent( "OnReport", {}, 0 );
	b.LdVar( n2 ); b.CallCmd( repf );
	b.LdVar( s2 ); b.CallCmd( rep );
	b.Op( OP_END );
	b.EndEntry();
	ats_program* pb = env.Load( b );
	REQUIRE( pb );

	env.MakeVm();
	ats_vm_attach( env.vm, pb );
	ats_vm_load( env.vm, MemoryFile::Read, &f );
	CHECK_EQ( RunReport( env, pb ), std::string( "5,7" ) );	// 変換できない mode は初期値
	CHECK_EQ( env.warnings.size(), (size_t)1 );
	ats_program_release( pb );
}

TEST( LoadAbortsRunningFibers )
{
	Env env;
	env.RegisterReporters();
	DefineShared( env );
	env.MakeVm();
	ats_program* prog = LoadQuest( env );
	REQUIRE( prog );
	MemoryFile f = Save( env );

	ProgramWriter w( "test/sleeper" );
	w.BeginEvent( "OnStart", {}, 0 );
	w.PushF( 10.0f ); w.Op( OP_SLEEP ); w.Op( OP_END );
	w.EndEntry();
	ats_program* sleeper = env.Load( w );
	REQUIRE( sleeper );
	env.Fire( sleeper, "OnStart" );
	env.Update();
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 1 );
	CHECK_EQ( ats_vm_load( env.vm, MemoryFile::Read, &f ), ATS_OK );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );		// 実行中のファイバは保存も復元もしない
	ats_program_release( prog );
	ats_program_release( sleeper );
}

TEST( CorruptSaveIsRejected )
{
	Env env;
	env.RegisterReporters();
	DefineShared( env );
	env.MakeVm();
	ats_program* prog = LoadQuest( env );
	REQUIRE( prog );
	env.Fire( prog, "OnSet" );
	env.Update();
	MemoryFile f = Save( env );

	// 途中で切れたデータ：失敗し、現在の状態は変わらない
	MemoryFile cut;
	cut.data.assign( f.data.begin(), f.data.begin() + (long)(f.data.size() / 2) );
	CHECK( ats_vm_load( env.vm, MemoryFile::Read, &cut ) != ATS_OK );
	CHECK_EQ( RunReport( env, prog ), std::string( "3,castle,true,2,5,lab" ) );

	MemoryFile bad = f;
	bad.data[0] = 'X';
	CHECK_EQ( ats_vm_load( env.vm, MemoryFile::Read, &bad ), ATS_ERR_BAD_FORMAT );

	// どのバイトを壊しても落ちない
	for( size_t i = 0; i < f.data.size(); ++i ){
		MemoryFile m = f;
		m.data[i] ^= 0x5A;
		ats_vm_load( env.vm, MemoryFile::Read, &m );
	}
	ats_program_release( prog );
}

#if defined(ATS_ENABLE_DEBUG)
TEST( DebugScriptVarAccess )
{
	Env env;
	env.RegisterReporters();
	DefineShared( env );
	env.MakeVm();
	ats_program* prog = LoadQuest( env );
	REQUIRE( prog );
	CHECK_EQ( ats_vm_attach( env.vm, prog ), ATS_OK );

	ats_value v = V_Int( 8 );
	CHECK_EQ( ats_debug_script_var_set( env.vm, "golden_lore/el_rescue", "retry", &v ), ATS_OK );
	ats_value out;
	CHECK_EQ( ats_debug_script_var_get( env.vm, "golden_lore/el_rescue", "retry", &out ), ATS_OK );
	CHECK_EQ( out.v.i, 8 );
	CHECK_EQ( ats_debug_script_var_get( env.vm, "golden_lore/el_rescue", "nope", &out ), ATS_ERR_NOT_FOUND );
	ats_program_release( prog );
}
#endif
