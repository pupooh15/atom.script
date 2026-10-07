/**************************************************************************/
/*!	\file	test_commands.cpp
	\brief	コマンド呼び出し（待機・再呼び出し・失敗・チャンネル・キャンセル）
***************************************************************************/
#include "test_framework.h"

using namespace ats::fmt;
using ats::writer::ProgramWriter;

namespace {

// ホスト側の待機中コマンドの記録
struct Host {
	std::vector<ats_call_token>	tokens;
	std::vector<int>			args;
	std::vector<ats_call_token>	cancelled;
	int							runningUntil = 0;
	bool						abortSelf = false;
	bool						completeNow = false;
};

ats_status ATS_CALL PendingFn( ats_call* call, void* user )
{
	Host* h = static_cast<Host*>( user );
	h->args.push_back( ats_call_argc( call ) ? ats_arg_int( call, 0 ) : 0 );
	ats_call_token t = ats_call_get_token( call );
	h->tokens.push_back( t );
	if( h->completeNow ){
		ats_value v = V_Int( 99 );
		ats_call_complete( ats_call_vm( call ), t, &v );
	}
	return ATS_PENDING;
}

void ATS_CALL CancelFn( ats_vm*, ats_call_token token, void* user )
{
	static_cast<Host*>( user )->cancelled.push_back( token );
}

ats_status ATS_CALL RunningFn( ats_call* call, void* user )
{
	Host* h = static_cast<Host*>( user );
	if( (int)ats_call_retry_count( call ) < h->runningUntil ) return ATS_RUNNING;
	ats_value v = V_Int( (int)ats_call_retry_count( call ) );
	ats_call_set_result( call, &v );
	return ATS_DONE;
}

ats_status ATS_CALL FailFn( ats_call*, void* ) { return ATS_FAIL; }

ats_status ATS_CALL AbortSelfFn( ats_call* call, void* )
{
	ats_vm_abort_fiber( ats_call_vm( call ), ats_call_fiber( call ) );
	return ATS_DONE;
}

ats_result ATS_CALL FailingQuery( ats_call*, void* ) { return ATS_ERR_NOT_FOUND; }

void RegCommand( Env& env, const char* name, ats_command_fn fn, Host* h,
				 const char* channel = nullptr, ats_cancel_fn cancel = nullptr )
{
	ats_command_desc d{};
	d.size    = sizeof(d);
	d.name    = name;
	d.channel = channel;
	d.fn      = fn;
	d.cancel  = cancel;
	d.user    = h;
	ats_register_command( env.rt, &d );
}

}	// namespace

//=========================================================================
// ATS_PENDING
//=========================================================================
TEST( PendingCommandWaitsForCompletion )
{
	Host host;
	Env env;
	env.RegisterReporters();
	RegCommand( env, "Wait", PendingFn, &host );
	env.MakeVm();

	ProgramWriter w( "test/pending" );
	uint16_t wait = w.Command( "Wait", { ATS_TYPE_INT }, ATS_TYPE_INT );
	uint16_t rep  = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	w.PushI( 5 ); w.CallCmd( wait ); w.CallCmd( rep );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );

	ats_fiber_id fid = 0;
	ats_vm_fire_event( env.vm, prog, "OnStart", nullptr, 0, &fid );
	env.Update( 1.0f / 60, 3 );
	CHECK( env.reports.empty() );
	CHECK( ats_vm_is_fiber_alive( env.vm, fid ) );
	REQUIRE( host.tokens.size() == 1 );
	CHECK_EQ( host.args[0], 5 );

	ats_value v = V_Int( 7 );
	CHECK_EQ( ats_call_complete( env.vm, host.tokens[0], &v ), ATS_OK );
	CHECK_EQ( ats_call_complete( env.vm, host.tokens[0], &v ), ATS_ERR_STALE_ID );	// 2 回目は無効
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "7" ) );
	CHECK( !ats_vm_is_fiber_alive( env.vm, fid ) );
	ats_program_release( prog );
}

TEST( CompleteInsideHandler )
{
	Host host;
	Env env;
	host.completeNow = true;
	env.RegisterReporters();
	RegCommand( env, "Wait", PendingFn, &host );
	env.MakeVm();

	ProgramWriter w( "test/complete_now" );
	uint16_t wait = w.Command( "Wait", { ATS_TYPE_INT }, ATS_TYPE_INT );
	uint16_t rep  = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	w.PushI( 1 ); w.CallCmd( wait ); w.CallCmd( rep );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "99" ) );		// 同じ update で続きまで進む
	ats_program_release( prog );
}

//=========================================================================
// ATS_RUNNING
//=========================================================================
TEST( RunningCommandIsCalledEveryUpdate )
{
	Host host;
	Env env;
	host.runningUntil = 2;
	env.RegisterReporters();
	RegCommand( env, "Fade", RunningFn, &host );
	env.MakeVm();

	ProgramWriter w( "test/running" );
	uint16_t fade = w.Command( "Fade", {}, ATS_TYPE_INT );
	uint16_t rep  = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	w.CallCmd( fade ); w.CallCmd( rep );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );

	env.Update();	// 初回呼び出し（retry 0）→ RUNNING
	env.Update();	// retry 1 → RUNNING
	CHECK( env.reports.empty() );
	env.Update();	// retry 2 → DONE
	CHECK_EQ( env.Reports(), std::string( "2" ) );
	ats_program_release( prog );
}

//=========================================================================
// 失敗
//=========================================================================
TEST( FailedCommandAbortsFiber )
{
	Host host;
	Env env;
	env.RegisterReporters();
	RegCommand( env, "Broken", FailFn, &host );
	RegCommand( env, "Wait", PendingFn, &host );
	env.MakeVm();

	ProgramWriter w( "test/fail" );
	uint16_t broken = w.Command( "Broken", {} );
	uint16_t wait   = w.Command( "Wait", { ATS_TYPE_INT } );
	uint16_t rep    = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnBroken", {}, 0 );
	w.CallCmd( broken ); w.PushI( 1 ); w.CallCmd( rep ); w.Op( OP_END );
	w.EndEntry();
	w.BeginEvent( "OnWait", {}, 0 );
	w.PushI( 0 ); w.CallCmd( wait ); w.PushI( 2 ); w.CallCmd( rep ); w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );

	env.Fire( prog, "OnBroken" );
	env.Update();
	CHECK( env.reports.empty() );
	REQUIRE( env.errors.size() == 1 );
	CHECK( env.errors[0].find( "Broken" ) != std::string::npos );

	// 待機中に ats_call_fail
	env.Fire( prog, "OnWait" );
	env.Update();
	REQUIRE( host.tokens.size() == 1 );
	CHECK_EQ( ats_call_fail( env.vm, host.tokens[0], "timeout" ), ATS_OK );
	env.Update();
	CHECK( env.reports.empty() );
	CHECK_EQ( env.errors.size(), (size_t)2 );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	ats_program_release( prog );
}

TEST( FailedQueryAbortsFiber )
{
	Env env;
	env.RegisterReporters();
	ats_query_desc q{};
	q.size = sizeof(q);
	q.name = "Lookup";
	q.fn   = FailingQuery;
	ats_register_query( env.rt, &q );
	env.MakeVm();

	ProgramWriter w( "test/qfail" );
	uint16_t look = w.Query( "Lookup", {}, ATS_TYPE_INT );
	uint16_t rep  = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	w.CallQuery( look ); w.CallCmd( rep ); w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );
	env.Update();
	CHECK( env.reports.empty() );
	CHECK_EQ( env.errors.size(), (size_t)1 );
	ats_program_release( prog );
}

//=========================================================================
// チャンネル（同じチャンネルのコマンドは 1 つずつ）
//=========================================================================
TEST( ChannelSerializesCommands )
{
	Host host;
	Env env;
	env.RegisterReporters();
	RegCommand( env, "ShowMessage", PendingFn, &host, "message" );
	env.MakeVm();

	ProgramWriter w( "test/channel" );
	uint16_t msg = w.Command( "ShowMessage", { ATS_TYPE_INT } );
	uint16_t rep = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnTalk", { ATS_TYPE_INT }, 1 );
	w.LdLocal( 0 ); w.CallCmd( msg );
	w.LdLocal( 0 ); w.CallCmd( rep );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );

	env.Fire( prog, "OnTalk", { V_Int( 1 ) } );
	env.Fire( prog, "OnTalk", { V_Int( 2 ) } );
	env.Update();
	REQUIRE( host.tokens.size() == 1 );		// 2 本目はチャンネル待ち
	CHECK_EQ( host.args[0], 1 );

	ats_call_complete( env.vm, host.tokens[0], nullptr );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "1" ) );
	REQUIRE( host.tokens.size() == 2 );
	CHECK_EQ( host.args[1], 2 );

	ats_call_complete( env.vm, host.tokens[1], nullptr );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "1,2" ) );
	ats_program_release( prog );
}

TEST( ChannelReleasedWhenOwnerAborted )
{
	Host host;
	Env env;
	RegCommand( env, "ShowMessage", PendingFn, &host, "message", CancelFn );
	env.MakeVm();

	ProgramWriter w( "test/channel_abort" );
	uint16_t msg = w.Command( "ShowMessage", { ATS_TYPE_INT } );
	w.BeginEvent( "OnTalk", { ATS_TYPE_INT }, 1 );
	w.LdLocal( 0 ); w.CallCmd( msg ); w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );

	ats_fiber_id a = 0;
	ats_value arg = V_Int( 1 );
	ats_vm_fire_event( env.vm, prog, "OnTalk", &arg, 1, &a );
	env.Fire( prog, "OnTalk", { V_Int( 2 ) } );
	env.Update();
	REQUIRE( host.tokens.size() == 1 );

	CHECK_EQ( ats_vm_abort_fiber( env.vm, a ), ATS_OK );
	REQUIRE( host.cancelled.size() == 1 );
	CHECK_EQ( host.cancelled[0], host.tokens[0] );		// キャンセル通知に同じトークンが来る
	env.Update();
	REQUIRE( host.tokens.size() == 2 );					// 待っていた 2 本目が動き出す
	CHECK_EQ( host.args[1], 2 );
	CHECK_EQ( ats_vm_abort_fiber( env.vm, a ), ATS_ERR_STALE_ID );
	ats_program_release( prog );
}

//=========================================================================
// ハンドラの中からの中断
//=========================================================================
TEST( AbortFromInsideHandler )
{
	Host host;
	Env env;
	env.RegisterReporters();
	RegCommand( env, "Quit", AbortSelfFn, &host );
	env.MakeVm();

	ProgramWriter w( "test/abort_self" );
	uint16_t quit = w.Command( "Quit", {} );
	uint16_t rep  = w.Command( "Report", { ATS_TYPE_INT } );
	w.BeginEvent( "OnStart", {}, 0 );
	w.PushI( 1 ); w.CallCmd( rep );
	w.CallCmd( quit );
	w.PushI( 2 ); w.CallCmd( rep );
	w.Op( OP_END );
	w.EndEntry();
	ats_program* prog = env.Load( w );
	REQUIRE( prog );
	env.Fire( prog, "OnStart" );
	env.Update();
	CHECK_EQ( env.Reports(), std::string( "1" ) );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	CHECK( env.errors.empty() );
	ats_program_release( prog );
}

TEST( DestroyCancelsPendingCommands )
{
	Host host;
	{
		Env env;
		RegCommand( env, "Wait", PendingFn, &host, nullptr, CancelFn );
		env.MakeVm();
		ProgramWriter w( "test/destroy" );
		uint16_t wait = w.Command( "Wait", { ATS_TYPE_INT } );
		w.BeginEvent( "OnStart", {}, 0 );
		w.PushI( 0 ); w.CallCmd( wait ); w.Op( OP_END );
		w.EndEntry();
		ats_program* prog = env.Load( w );
		REQUIRE( prog );
		env.Fire( prog, "OnStart" );
		env.Update();
		ats_program_release( prog );	// VM が参照を持っているので、まだ解放されない
	}
	CHECK_EQ( host.cancelled.size(), (size_t)1 );
}
