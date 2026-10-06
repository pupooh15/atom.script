/**************************************************************************/
/*!	\file	test_basic.cpp
	\brief	演算・分岐・関数・文字列・乱数・読み込み時の検証
***************************************************************************/
#include "test_framework.h"

using namespace ats::fmt;
using ats::writer::ProgramWriter;
using ats::writer::Label;

//=========================================================================
// 演算
//=========================================================================
TEST( Arithmetic )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter w( "test/arith" );
	uint16_t rep  = w.Command( "Report", { ATS_TYPE_INT } );
	uint16_t repf = w.Command( "ReportF", { ATS_TYPE_FLOAT } );
	w.BeginEvent( "OnStart", {}, 0 );
	w.PushI( 2 ); w.PushI( 3 ); w.Op( OP_ADD_I ); w.CallCmd( rep );					// 5
	w.PushI( 7 ); w.PushI( 3 ); w.Op( OP_SUB_I ); w.CallCmd( rep );					// 4
	w.PushI( 6 ); w.PushI( 7 ); w.Op( OP_MUL_I ); w.CallCmd( rep );					// 42
	w.PushI( -7 ); w.PushI( 2 ); w.Op( OP_DIV_I ); w.CallCmd( rep );				// -3
	w.PushI( -7 ); w.PushI( 2 ); w.Op( OP_MOD_I ); w.CallCmd( rep );				// -1
	w.PushI( 0x7FFFFFFF ); w.PushI( 1 ); w.Op( OP_ADD_I ); w.CallCmd( rep );		// 折り返し
	w.PushI( 1 ); w.PushI( 4 ); w.Op( OP_SHL ); w.CallCmd( rep );					// 16
	w.PushI( -16 ); w.PushI( 2 ); w.Op( OP_SHR ); w.CallCmd( rep );					// -4
	w.PushI( 0x0F ); w.PushI( 0x3C ); w.Op( OP_BAND ); w.CallCmd( rep );			// 12
	w.PushF( 1.5f ); w.PushF( 2.0f ); w.Op( OP_MUL_F ); w.CallCmd( repf );			// 3
	w.PushI( 7 ); w.Op( OP_I2F ); w.PushF( 2.0f ); w.Op( OP_DIV_F ); w.CallCmd( repf );	// 3.5
	w.PushF( -2.75f ); w.Op( OP_F2I ); w.CallCmd( rep );							// -2
	w.PushI( 3 ); w.PushI( 5 ); w.Op( OP_LT_I ); w.CallCmd( rep );					// 1
	w.PushI( 0 ); w.Op( OP_NOT ); w.CallCmd( rep );									// 1
	w.Op( OP_END );
	w.EndEntry();

	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "5,4,42,-3,-1,-2147483648,16,-4,12,3,3.5,-2,1,1" ) );
	CHECK( env.errors.empty() );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	ats_program_release( prog );
}

TEST( DivisionByZeroAbortsFiber )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter w( "test/div0" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	w.Line( 10 );
	w.PushI( 1 ); w.PushI( 0 ); w.Op( OP_DIV_I ); w.CallCmd( rep );
	w.Op( OP_END );
	w.EndEntry();

	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );
	env.Update();
	CHECK( env.reports.empty() );
	REQUIRE( env.errors.size() == 1 );
	CHECK( env.errors[0].find( "division by zero" ) != std::string::npos );
	CHECK( env.errors[0].find( "test/div0:10" ) != std::string::npos );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	ats_program_release( prog );
}

//=========================================================================
// 分岐
//=========================================================================
TEST( BranchAndSwitch )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter w( "test/branch" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnValue", { ATS_TYPE_INT }, 1 );
	{
		// if( v > 10 ) Report(100) else Report(200)
		Label els = w.NewLabel(), end = w.NewLabel();
		w.LdLocal( 0 ); w.PushI( 10 ); w.Op( OP_GT_I ); w.Jz( els );
		w.PushI( 100 ); w.CallCmd( rep ); w.Jmp( end );
		w.Bind( els );
		w.PushI( 200 ); w.CallCmd( rep );
		w.Bind( end );

		// switch( v ){ case 1: 1001; case 20: 1020; default: 9999 }
		Label c1 = w.NewLabel(), c20 = w.NewLabel(), def = w.NewLabel(), done = w.NewLabel();
		w.LdLocal( 0 );
		w.Switch( { { 1, c1 }, { 20, c20 } }, def );
		w.Bind( c1 );  w.PushI( 1001 ); w.CallCmd( rep ); w.Jmp( done );
		w.Bind( c20 ); w.PushI( 1020 ); w.CallCmd( rep ); w.Jmp( done );
		w.Bind( def ); w.PushI( 9999 ); w.CallCmd( rep );
		w.Bind( done );
		w.Op( OP_END );
	}
	w.EndEntry();

	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnValue", { V_Int( 1 ) } );
	env.Fire( prog, "OnValue", { V_Int( 20 ) } );
	env.Fire( prog, "OnValue", { V_Int( 5 ) } );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "200,1001,100,1020,200,9999" ) );
	ats_program_release( prog );
}

TEST( LoopWithBreak )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	// let i = 0; loop { if( i >= 3 ) break; Report( i ); i += 1 }
	ProgramWriter w( "test/loop" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 1 );
	Label top = w.NewLabel(), out = w.NewLabel();
	w.Bind( top );
	w.LdLocal( 0 ); w.PushI( 3 ); w.Op( OP_GE_I ); w.Jnz( out );
	w.LdLocal( 0 ); w.CallCmd( rep );
	w.LdLocal( 0 ); w.PushI( 1 ); w.Op( OP_ADD_I ); w.StLocal( 0 );
	w.Jmp( top );
	w.Bind( out );
	w.Op( OP_END );
	w.EndEntry();

	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "0,1,2" ) );
	ats_program_release( prog );
}

//=========================================================================
// 関数
//=========================================================================
TEST( FunctionCallAndReturn )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter w( "test/fn" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	uint16_t add = w.DeclareFunction( "Add3", { ATS_TYPE_INT, ATS_TYPE_INT }, ATS_TYPE_INT, 3 );
	uint16_t fac = w.DeclareFunction( "Fact", { ATS_TYPE_INT }, ATS_TYPE_INT, 1 );

	w.BeginEvent( "OnStart", {}, 0 );
	w.PushI( 10 ); w.PushI( 20 ); w.CallFn( add ); w.CallCmd( rep );		// 33
	w.PushI( 5 ); w.CallFn( fac ); w.CallCmd( rep );						// 120
	w.Op( OP_END );
	w.EndEntry();

	// Add3( a, b ) { let t = a + b; return t + 3 }
	w.BeginFunction( add );
	w.LdLocal( 0 ); w.LdLocal( 1 ); w.Op( OP_ADD_I ); w.StLocal( 2 );
	w.LdLocal( 2 ); w.PushI( 3 ); w.Op( OP_ADD_I ); w.Op( OP_RET );
	w.EndEntry();

	// Fact( n ) { if( n <= 1 ) return 1; return n * Fact( n - 1 ) }
	w.BeginFunction( fac );
	{
		Label rec = w.NewLabel();
		w.LdLocal( 0 ); w.PushI( 1 ); w.Op( OP_LE_I ); w.Jz( rec );
		w.PushI( 1 ); w.Op( OP_RET );
		w.Bind( rec );
		w.LdLocal( 0 ); w.LdLocal( 0 ); w.PushI( 1 ); w.Op( OP_SUB_I ); w.CallFn( fac ); w.Op( OP_MUL_I ); w.Op( OP_RET );
	}
	w.EndEntry();

	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "33,120" ) );
	ats_program_release( prog );
}

TEST( CallDepthLimit )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter w( "test/depth" );
	uint16_t fn = w.DeclareFunction( "Forever", {}, ATS_TYPE_VOID, 0 );
	w.BeginEvent( "OnStart", {}, 0 );
	w.CallFn( fn ); w.Op( OP_END );
	w.EndEntry();
	w.BeginFunction( fn );
	w.CallFn( fn ); w.Op( OP_RET );
	w.EndEntry();

	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );
	env.Update();
	REQUIRE( env.errors.size() == 1 );
	CHECK( env.errors[0].find( "call depth" ) != std::string::npos );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	ats_program_release( prog );
}

//=========================================================================
// 文字列・ハンドル
//=========================================================================
static ats_result ATS_CALL QueryName( ats_call* call, void* )
{
	std::string s = std::string( "hello " ) + ats_arg_string( call, 0 );
	ats_value v = V_Str( s.c_str() );		// 一時バッファでも VM がコピーする
	return ats_call_set_result( call, &v );
}

TEST( StringsAndHandles )
{
	Env env;
	env.RegisterReporters();
	ats_query_desc q{};
	q.size = sizeof(q);
	q.name = "Greet";
	q.fn   = QueryName;
	ats_register_query( env.rt, &q );
	env.MakeVm();

	ProgramWriter w( "test/str" );
	uint16_t reps  = w.Command( "ReportS", { ATS_TYPE_STRING } );
	uint16_t rep   = w.Command( "Report", { ATS_TYPE_INT } );
	uint16_t greet = w.Query( "Greet", { ATS_TYPE_STRING }, ATS_TYPE_STRING );
	w.BeginEvent( "OnTalk", { ATS_TYPE_HANDLE, ATS_TYPE_STRING }, 2 );
	w.PushStr( "world" ); w.CallQuery( greet ); w.Op( OP_DUP ); w.CallCmd( reps );		// hello world
	w.PushStr( "hello world" ); w.Op( OP_EQ_I ); w.CallCmd( rep );						// 文字列の一致 → 1
	w.LdLocal( 1 ); w.CallCmd( reps );													// 引数の文字列
	w.LdLocal( 0 ); w.PushK( 0x1234567890ull ); w.Op( OP_EQ_H ); w.CallCmd( rep );		// 1
	w.Op( OP_END );
	w.EndEntry();

	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnTalk", { V_Handle( 0x1234567890ull ), V_Str( "merchant" ) } );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "hello world,1,merchant,1" ) );
	CHECK( env.errors.empty() );
	ats_program_release( prog );
}

TEST( EventArgumentTypeChecked )
{
	Env env;
	env.MakeVm();
	ProgramWriter w( "test/argtype" );
	w.BeginEvent( "OnValue", { ATS_TYPE_INT }, 1 );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	ats_value args[] = { V_Str( "x" ) };
	CHECK_EQ( ats_vm_fire_event( env.vm, prog, "OnValue", args, 1, nullptr ), ATS_ERR_TYPE );
	CHECK_EQ( ats_vm_fire_event( env.vm, prog, "OnValue", args, 0, nullptr ), ATS_ERR_INVALID_ARG );
	CHECK_EQ( ats_vm_fire_event( env.vm, prog, "Missing", nullptr, 0, nullptr ), ATS_ERR_NOT_FOUND );
	ats_program_release( prog );
}

//=========================================================================
// 乱数（同じシードなら同じ結果）
//=========================================================================
static std::string RunRandom( uint64_t seed )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm( 0, seed );
	ProgramWriter w( "test/rand" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	for( int i = 0; i < 5; ++i ){ w.PushI( 100 ); w.Op( OP_RAND ); w.CallCmd( rep ); }
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	env.Fire( prog, "OnStart" );
	env.Update();
	ats_program_release( prog );
	return env.Reports();
}

TEST( RandomIsDeterministic )
{
	std::string a = RunRandom( 42 );
	std::string b = RunRandom( 42 );
	std::string c = RunRandom( 43 );
	CHECK_EQ( a, b );
	CHECK( a != c );
}

//=========================================================================
// 読み込み時の検証
//=========================================================================
TEST( UnresolvedImportsAreListed )
{
	Env env;
	ProgramWriter w( "test/unresolved" );
	uint16_t a = w.Command( "CameraShake", { ATS_TYPE_FLOAT } );
	uint16_t b = w.Query( "IsQuestCleared", { ATS_TYPE_INT }, ATS_TYPE_BOOL );
	uint16_t v = w.SharedVar( 1001, ATS_TYPE_INT );
	w.BeginEvent( "OnStart", {}, 0 );
	w.PushF( 1.0f ); w.CallCmd( a );
	w.PushI( 1 ); w.CallQuery( b ); w.Op( OP_POP );
	w.LdVar( v ); w.Op( OP_POP );
	w.Op( OP_END );
	w.EndEntry();

	ats_program* prog = nullptr;
	CHECK_EQ( env.LoadResult( w, &prog ), ATS_ERR_UNRESOLVED );
	std::string err = ats_runtime_last_error( env.rt );
	CHECK( err.find( "CameraShake" ) != std::string::npos );
	CHECK( err.find( "IsQuestCleared" ) != std::string::npos );
	CHECK( err.find( "1001" ) != std::string::npos );
	CHECK( prog == nullptr );
}

TEST( SignatureMismatch )
{
	Env env;
	ats_command_desc d{};
	d.size     = sizeof(d);
	d.name     = "ShowMessage";
	d.sig_hash = 0x1111;
	d.fn       = []( ats_call*, void* ) -> ats_status { return ATS_DONE; };
	ats_register_command( env.rt, &d );

	ProgramWriter w( "test/sig" );
	w.Command( "ShowMessage", { ATS_TYPE_STRING }, ATS_TYPE_VOID, 0x2222 );
	w.BeginEvent( "OnStart", {}, 0 );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = nullptr;
	CHECK_EQ( env.LoadResult( w, &prog ), ATS_ERR_SIGNATURE );
}

static ats_result LoadRaw( Env& env, const std::vector<uint8_t>& code, uint16_t localc = 0 )
{
	ProgramWriter w( "test/raw" );
	w.SetFixedMaxStack( 8 );
	w.BeginEvent( "OnStart", {}, localc );
	w.Raw( code );
	w.EndEntry();
	std::vector<uint8_t> bytes = w.Build();
	if( bytes.empty() ) return ATS_ERR_INVALID_ARG;
	ats_program* prog = nullptr;
	ats_result r = ats_program_load( env.rt, bytes.data(), bytes.size(), &prog );
	ats_program_release( prog );
	return r;
}

TEST( VerifierRejectsBadCode )
{
	Env env;
	// スタック不足
	CHECK_EQ( LoadRaw( env, { OP_POP, OP_END } ), ATS_ERR_BAD_FORMAT );
	// 範囲外へのジャンプ
	CHECK_EQ( LoadRaw( env, { OP_JMP, 0x00, 0x10, 0x00, 0x00, OP_END } ), ATS_ERR_BAD_FORMAT );
	// 範囲外のローカル
	CHECK_EQ( LoadRaw( env, { OP_LD_LOCAL, 0x05, 0x00, OP_POP, OP_END }, 1 ), ATS_ERR_BAD_FORMAT );
	// 未知のオペコード
	CHECK_EQ( LoadRaw( env, { 0xEE } ), ATS_ERR_BAD_FORMAT );
	// 命令の途中で終わっている
	CHECK_EQ( LoadRaw( env, { OP_PUSH_I32, 0x01 } ), ATS_ERR_BAD_FORMAT );
	// 正しいコードは通る
	CHECK_EQ( LoadRaw( env, { OP_PUSH_I32, 1, 0, 0, 0, OP_POP, OP_END } ), ATS_OK );
}

TEST( VerifierRejectsCorruptHeader )
{
	Env env;
	ProgramWriter w( "test/corrupt" );
	w.BeginEvent( "OnStart", {}, 0 );
	w.Op( OP_END );
	w.EndEntry();
	std::vector<uint8_t> bytes = w.Build();
	REQUIRE( !bytes.empty() );

	ats_program* prog = nullptr;
	std::vector<uint8_t> bad = bytes;
	bad[0] = 'X';
	CHECK_EQ( ats_program_load( env.rt, bad.data(), bad.size(), &prog ), ATS_ERR_BAD_FORMAT );

	bad = bytes;
	bad[4] = 2;		// major バージョン
	CHECK_EQ( ats_program_load( env.rt, bad.data(), bad.size(), &prog ), ATS_ERR_VERSION );

	CHECK_EQ( ats_program_load( env.rt, bytes.data(), bytes.size() / 2, &prog ), ATS_ERR_BAD_FORMAT );

	// どのバイトを壊しても落ちない（簡易ファジング）
	for( size_t i = 0; i < bytes.size(); ++i ){
		for( int bit = 0; bit < 8; ++bit ){
			bad = bytes;
			bad[i] ^= (uint8_t)(1 << bit);
			ats_program* p = nullptr;
			if( ats_program_load( env.rt, bad.data(), bad.size(), &p ) == ATS_OK ) ats_program_release( p );
		}
	}
	CHECK( ats_program_load( env.rt, bytes.data(), bytes.size(), &prog ) == ATS_OK );
	ats_program_release( prog );
}
