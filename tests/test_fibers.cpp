/**************************************************************************/
/*!	\file	test_fibers.cpp
	\brief	ファイバ（並行・待機・命令数上限・イベント発火・切り離し）
***************************************************************************/
#include "test_framework.h"

using namespace ats::fmt;
using ats::writer::ProgramWriter;
using ats::writer::Label;

namespace {

struct Pending {
	std::vector<ats_call_token>	tokens;
	std::vector<ats_call_token>	cancelled;
};

ats_status ATS_CALL WaitFn( ats_call* call, void* user )
{
	static_cast<Pending*>( user )->tokens.push_back( ats_call_get_token( call ) );
	return ATS_PENDING;
}

void ATS_CALL CancelFn( ats_vm*, ats_call_token t, void* user )
{
	static_cast<Pending*>( user )->cancelled.push_back( t );
}

}	// namespace

//=========================================================================
// 待機
//=========================================================================
TEST( SleepYieldWaitFrames )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter w( "test/wait" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	w.PushI( 1 ); w.CallCmd( rep );
	w.Op( OP_YIELD );
	w.PushI( 2 ); w.CallCmd( rep );
	w.PushF( 0.5f ); w.Op( OP_SLEEP );
	w.PushI( 3 ); w.CallCmd( rep );
	w.PushI( 2 ); w.Op( OP_WAIT_FRAMES );
	w.PushI( 4 ); w.CallCmd( rep );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );

	env.Update( 0.25f );	CHECK_EQ( env.Reports(), std::string( "1" ) );
	env.Update( 0.25f );	CHECK_EQ( env.Reports(), std::string( "1,2" ) );		// yield の次のフレーム
	env.Update( 0.25f );	CHECK_EQ( env.Reports(), std::string( "1,2" ) );		// 0.25 / 0.5 秒
	env.Update( 0.25f );	CHECK_EQ( env.Reports(), std::string( "1,2,3" ) );
	env.Update( 0.25f );	CHECK_EQ( env.Reports(), std::string( "1,2,3" ) );
	env.Update( 0.25f );	CHECK_EQ( env.Reports(), std::string( "1,2,3,4" ) );
	ats_program_release( prog );
}

//=========================================================================
// parallel { branch { … } branch { … } }
//=========================================================================
TEST( ForkJoinWaitsForAllBranches )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter w( "test/join" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 1 );
	Label b1 = w.NewLabel(), b2 = w.NewLabel(), join = w.NewLabel();
	w.PushI( 7 ); w.StLocal( 0 );			// 子ファイバはローカルのコピーを持つ
	w.Fork( b1 );
	w.Fork( b2 );
	w.Jmp( join );
	w.Bind( b1 );
	w.PushF( 0.1f ); w.Op( OP_SLEEP ); w.PushI( 10 ); w.CallCmd( rep ); w.Op( OP_END );
	w.Bind( b2 );
	w.PushF( 0.3f ); w.Op( OP_SLEEP ); w.LdLocal( 0 ); w.CallCmd( rep ); w.Op( OP_END );
	w.Bind( join );
	w.Op( OP_JOIN );
	w.PushI( 99 ); w.CallCmd( rep );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );

	env.Update( 0.0f );		CHECK( env.reports.empty() );
	env.Update( 0.2f );		CHECK_EQ( env.Reports(), std::string( "10" ) );
	env.Update( 0.2f );		CHECK_EQ( env.Reports(), std::string( "10,7,99" ) );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	ats_program_release( prog );
}

TEST( RaceAbortsTheOtherBranches )
{
	Env env;
	Pending p;
	env.RegisterReporters();
	ats_command_desc d{};
	d.size   = sizeof(d);
	d.name   = "WaitInput";
	d.fn     = WaitFn;
	d.cancel = CancelFn;
	d.user   = &p;
	ats_register_command( env.rt, &d );
	env.MakeVm();

	ProgramWriter w( "test/race" );
	uint16_t rep  = w.Command( "Report", { ATS_TYPE_INT } );
	uint16_t wait = w.Command( "WaitInput", {} );
	w.BeginEvent( "OnStart", {}, 0 );
	Label b1 = w.NewLabel(), b2 = w.NewLabel(), race = w.NewLabel();
	w.Fork( b1 );
	w.Fork( b2 );
	w.Jmp( race );
	w.Bind( b1 );		// タイムアウト
	w.PushF( 1.0f ); w.Op( OP_SLEEP ); w.PushI( 1 ); w.CallCmd( rep ); w.Op( OP_END );
	w.Bind( b2 );		// 入力待ち
	w.CallCmd( wait ); w.PushI( 2 ); w.CallCmd( rep ); w.Op( OP_END );
	w.Bind( race );
	w.Op( OP_RACE );
	w.PushI( 99 ); w.CallCmd( rep );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );

	env.Update( 0.0f );		// 子ファイバが動き出す（スリープ開始）
	env.Update( 0.5f );
	CHECK( env.reports.empty() );
	REQUIRE( p.tokens.size() == 1 );
	env.Update( 0.6f );		// タイムアウト側が先に終わる
	CHECK_EQ( env.Reports(), std::string( "1,99" ) );
	REQUIRE( p.cancelled.size() == 1 );		// 入力待ちはキャンセルされる
	CHECK_EQ( p.cancelled[0], p.tokens[0] );
	CHECK_EQ( ats_call_complete( env.vm, p.tokens[0], nullptr ), ATS_ERR_STALE_ID );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	ats_program_release( prog );
}

TEST( AbortParentAbortsChildren )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter w( "test/abort_tree" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	Label b1 = w.NewLabel(), join = w.NewLabel();
	w.Fork( b1 );
	w.Jmp( join );
	w.Bind( b1 );
	w.PushF( 1.0f ); w.Op( OP_SLEEP ); w.PushI( 1 ); w.CallCmd( rep ); w.Op( OP_END );
	w.Bind( join );
	w.Op( OP_JOIN );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );

	ats_fiber_id parent = 0;
	ats_vm_fire_event( env.vm, prog, "OnStart", nullptr, 0, &parent );
	env.Update( 0.1f );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 2 );
	ats_vm_abort_fiber( env.vm, parent );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	env.Update( 2.0f );
	CHECK( env.reports.empty() );
	ats_program_release( prog );
}

//=========================================================================
// 命令数の上限（無限ループでも固まらない）
//=========================================================================
TEST( InstructionBudget )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm( 1000 );

	ProgramWriter w( "test/budget" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnLoop", {}, 0 );
	Label top = w.NewLabel();
	w.Bind( top );
	w.Jmp( top );
	w.EndEntry();
	w.BeginEvent( "OnOther", {}, 0 );
	w.PushI( 1 ); w.CallCmd( rep ); w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );

	ats_fiber_id loop = 0;
	ats_vm_fire_event( env.vm, prog, "OnLoop", nullptr, 0, &loop );
	env.Fire( prog, "OnOther" );
	env.Update();
	CHECK( !env.warnings.empty() );
	CHECK( ats_vm_is_fiber_alive( env.vm, loop ) );
	ats_vm_abort_fiber( env.vm, loop );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "1" ) );
	ats_program_release( prog );
}

//=========================================================================
// fire（別スクリプトのイベントを発火）
//=========================================================================
TEST( FireEventInOtherScript )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter a( "test/fire_a" );
	a.BeginEvent( "OnStart", {}, 0 );
	a.PushI( 42 ); a.Fire( "OnSignal", 1 );
	a.Op( OP_END );
	a.EndEntry();

	ProgramWriter b( "test/fire_b" );
	uint16_t rep = b.Command( "Report", { ATS_TYPE_INT } );
	b.BeginEvent( "OnSignal", { ATS_TYPE_INT }, 1 );
	b.LdLocal( 0 ); b.CallCmd( rep ); b.Op( OP_END );
	b.EndEntry();

	ats_program* pa = env.Load( a );
	ats_program* pb = env.Load( b );
	REQUIRE( pa && pb );
	CHECK_EQ( ats_vm_attach( env.vm, pb ), ATS_OK );
	env.Fire( pa, "OnStart" );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "42" ) );

	int count = 0;
	ats_value arg = V_Int( 5 );
	CHECK_EQ( ats_vm_broadcast_event( env.vm, "OnSignal", &arg, 1, &count ), ATS_OK );
	CHECK_EQ( count, 1 );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "42,5" ) );
	ats_program_release( pa );
	ats_program_release( pb );
}

//=========================================================================
// 切り離し（実行中のファイバは中断、プログラムは参照が切れたら解放）
//=========================================================================
TEST( DetachAbortsFibers )
{
	Env env;
	env.RegisterReporters();
	env.MakeVm();

	ProgramWriter w( "test/detach" );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	w.PushF( 1.0f ); w.Op( OP_SLEEP ); w.PushI( 1 ); w.CallCmd( rep ); w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );
	env.Update( 0.1f );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 1 );
	CHECK_EQ( ats_vm_detach( env.vm, prog ), ATS_OK );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	CHECK_EQ( ats_vm_detach( env.vm, prog ), ATS_ERR_NOT_FOUND );
	env.Update( 2.0f );
	CHECK( env.reports.empty() );
	ats_program_release( prog );
}
