/**************************************************************************/
/*!	\file	ats_vm.cpp
	\brief	VM（ファイバ・命令実行・コマンド呼び出し）
***************************************************************************/
#include "ats_internal.h"

#include <math.h>

using namespace ats;
using namespace ats::fmt;

namespace {

constexpr uint32_t kDefaultBudget		= 100000;
constexpr uint32_t kDefaultMaxStack		= 1024;
constexpr uint32_t kDefaultMaxDepth		= 64;
constexpr uint32_t kNoReturn			= 0xFFFFFFFFu;

template<class T> T Read( const uint8_t* p ) { T v; memcpy( &v, p, sizeof(T) ); return v; }

uint64_t NextRandom( uint64_t& s )
{
	// xorshift64*
	s ^= s >> 12;
	s ^= s << 25;
	s ^= s >> 27;
	return s * 2685821657736338717ull;
}

}	// namespace

//=========================================================================
// ログ・文字列・値
//=========================================================================
void ats_vm::Log( ats_log_level level, const char* fmt, ... )
{
	va_list ap;
	va_start( ap, fmt );
	FormatV( error, sizeof(error), fmt, ap );
	va_end( ap );
	if( rt->log ) rt->log( level, error, rt->log_user );
}

uint32_t ats_vm::Intern( const char* s )
{
	if( !s ) s = "";
	uint32_t id;
	if( intern_map.Find( s, &id ) ) return id;
	char* d = alloc->StrDup( s );
	if( !d || !interned.Push( d ) ){ alloc->Free( d ); return 0; }
	id = interned.Size() - 1;
	if( !intern_map.Insert( d, id ) ) return 0;
	return id;
}

uint64_t ats_vm::ToSlot( const ats_value& v )
{
	switch( v.type ){
	case ATS_TYPE_BOOL:		return v.v.b ? 1 : 0;
	case ATS_TYPE_INT:
	case ATS_TYPE_ENUM:		return FromI( v.v.i );
	case ATS_TYPE_FLOAT:	return FromF( v.v.f );
	case ATS_TYPE_STRING:	return Intern( v.v.s );
	case ATS_TYPE_HANDLE:	return v.v.h;
	}
	return 0;
}

ats_value ats_vm::FromSlot( uint64_t s, uint32_t type ) const
{
	ats_value v;
	memset( &v, 0, sizeof(v) );
	v.type = type;
	switch( type ){
	case ATS_TYPE_BOOL:		v.v.b = s ? 1 : 0; break;
	case ATS_TYPE_INT:
	case ATS_TYPE_ENUM:		v.v.i = SlotI( s ); break;
	case ATS_TYPE_FLOAT:	v.v.f = SlotF( s ); break;
	case ATS_TYPE_STRING:	v.v.s = Str( s ); break;
	case ATS_TYPE_HANDLE:	v.v.h = s; break;
	}
	return v;
}

// 型が合うか（INT と ENUM は相互に代入できる）
static bool TypeMatch( uint32_t want, uint32_t have )
{
	return want == have || (IsIntLike( want ) && IsIntLike( have ));
}

//=========================================================================
// ファイバ
//=========================================================================
ats_fiber_id ats_vm::FiberId( const Fiber* f ) const
{
	return ((uint64_t)f->generation << 32) | (uint64_t)(f->index + 1);
}

Fiber* ats_vm::GetFiber( ats_fiber_id id )
{
	uint32_t idx = (uint32_t)id;
	if( idx == 0 || idx > fibers.Size() ) return nullptr;
	Fiber* f = fibers[ idx - 1 ];
	if( f->state == FiberState::Free || f->generation != (uint32_t)(id >> 32) ) return nullptr;
	return f;
}

static Fiber* AllocFiber( ats_vm* vm )
{
	for( uint32_t i = 0; i < vm->fibers.Size(); ++i )
		if( vm->fibers[i]->state == FiberState::Free ) return vm->fibers[i];

	Fiber* f = vm->alloc->New<Fiber>();
	if( !f ) return nullptr;
	memset( (void*)&f->call, 0, sizeof(ats_call) - sizeof(Array<ats_value>) );
	f->state      = FiberState::Free;
	f->generation = 1;
	f->index      = vm->fibers.Size();
	f->stack.Init( vm->alloc );
	f->callstack.Init( vm->alloc );
	f->children.Init( vm->alloc );
	f->call.args.Init( vm->alloc );
	if( !f->stack.Reserve( vm->max_stack ) || !f->callstack.Reserve( vm->max_depth ) || !vm->fibers.Push( f ) ){
		vm->alloc->Delete( f );
		return nullptr;
	}
	return f;
}

// entry のフレームを 1 つ持つファイバを作る（ローカルは 0 で初期化）
Fiber* ats_vm::NewFiber( Binding* b, uint32_t entry )
{
	const EntryEntry& e = b->prog->entries[ entry ];
	if( (uint32_t)e.localc + e.max_stack > max_stack ){
		Log( ATS_LOG_ERROR, "[%s] entry '%s' needs more stack than max_stack", b->prog->script_id, b->prog->strings[ e.name ] );
		return nullptr;
	}
	Fiber* f = AllocFiber( this );
	if( !f ){ Log( ATS_LOG_ERROR, "out of memory (fiber)" ); return nullptr; }

	f->state             = FiberState::Ready;
	f->binding           = b;
	f->pc                = e.code;
	f->parent            = 0;
	f->parent_gen        = 0;
	f->live_children     = 0;
	f->finished_children = 0;
	f->join_mode         = 0;
	f->abort_requested   = false;
	f->sleep             = 0.0f;
	f->frames            = 0;
	f->stack.Clear();
	f->callstack.Clear();
	f->children.Clear();
	f->call.vm            = this;
	f->call.fiber_index   = f->index;
	f->call.active        = false;
	f->call.holds_channel = false;
	f->call.token_seq     = 0;
	f->stack.Resize( e.localc );

	Frame fr;
	fr.entry       = entry;
	fr.ret_pc      = kNoReturn;
	fr.locals_base = 0;
	f->callstack.Push( fr );
	return f;
}

ats_result ats_vm::StartEvent( Binding* b, const char* event, const ats_value* args, int argc, ats_fiber_id* out )
{
	int entry = b->prog->FindEntry( event, kEntryEvent );
	if( entry < 0 ) return ATS_ERR_NOT_FOUND;
	const EntryEntry& e = b->prog->entries[ (uint32_t)entry ];
	if( argc != e.paramc ){
		Log( ATS_LOG_ERROR, "[%s] event '%s' expects %d args (got %d)", b->prog->script_id, event, e.paramc, argc );
		return ATS_ERR_INVALID_ARG;
	}
	for( int i = 0; i < argc; ++i ){
		if( !TypeMatch( e.param_types[i], args[i].type ) ){
			Log( ATS_LOG_ERROR, "[%s] event '%s' arg %d: type mismatch", b->prog->script_id, event, i );
			return ATS_ERR_TYPE;
		}
	}
	Fiber* f = NewFiber( b, (uint32_t)entry );
	if( !f ) return ATS_ERR_OUT_OF_MEMORY;
	for( int i = 0; i < argc; ++i ) f->stack[ (uint32_t)i ] = ToSlot( args[i] );
	if( out ) *out = FiberId( f );
	return ATS_OK;
}

void ats_vm::ReleaseChannel( Fiber* f )
{
	if( !f->call.holds_channel ) return;
	f->call.holds_channel = false;
	const ats_program* prog = f->binding->prog;
	const CommandDef& cmd = rt->commands[ prog->resolved[ f->call.import ].index ];
	Channel* ch = channels[ cmd.channel ];
	ch->owner = 0;
	// 待っている次のファイバを起こす
	while( !ch->waiters.Empty() ){
		ats_fiber_id id = ch->waiters[0];
		ch->waiters.RemoveAt( 0 );
		Fiber* w = GetFiber( id );
		if( w && w->state == FiberState::WaitChannel ){
			w->state = FiberState::Ready;
			break;
		}
	}
}

// 子ファイバが終わったことを親に伝える
static void NotifyParent( ats_vm* vm, Fiber* child )
{
	if( !child->parent ) return;
	Fiber* p = vm->GetFiber( ((uint64_t)child->parent_gen << 32) | child->parent );
	child->parent = 0;
	if( !p || p->state == FiberState::Dying ) return;

	for( uint32_t i = 0; i < p->children.Size(); ++i ){
		if( p->children[i] == child->index ){ p->children.RemoveAt( i ); break; }
	}
	--p->live_children;
	++p->finished_children;

	if( p->state != FiberState::WaitJoin ) return;
	if( p->join_mode == OP_RACE ){
		// 1 本終わったら残りを中断する
		while( !p->children.Empty() ){
			Fiber* c = vm->fibers[ p->children.Back() ];
			vm->AbortFiber( c, true );
			if( !p->children.Empty() && p->children.Back() == c->index ) p->children.Pop();	// 中断が保留された場合
		}
	}
	if( p->live_children == 0 ){
		p->finished_children = 0;
		p->state = FiberState::Ready;
	}
}

void ats_vm::FinishFiber( Fiber* f )
{
	f->state = FiberState::Dying;

	// 子ファイバを中断する
	while( !f->children.Empty() ){
		uint32_t ci = f->children.Back();
		f->children.Pop();
		Fiber* c = fibers[ ci ];
		c->parent = 0;		// 親への通知は不要
		AbortFiber( c, true );
	}

	ReleaseChannel( f );
	f->call.active = false;
	NotifyParent( this, f );

	f->stack.Clear();
	f->callstack.Clear();
	f->binding = nullptr;
	f->state   = FiberState::Free;
	++f->generation;
	if( f->generation == 0 ) f->generation = 1;
}

void ats_vm::AbortFiber( Fiber* f, bool cancel )
{
	if( f->state == FiberState::Free || f->state == FiberState::Dying ) return;
	// 実行中のファイバ（ハンドラの中から中断された場合）は、ハンドラから戻った後で処理する
	if( current == f->index + 1 ){
		f->abort_requested = true;
		return;
	}
	if( cancel && f->call.active &&
		(f->state == FiberState::WaitCall || f->state == FiberState::CallRunning) ){
		const ats_program* prog = f->binding->prog;
		const CommandDef& cmd = rt->commands[ prog->resolved[ f->call.import ].index ];
		if( cmd.cancel ){
			ats_call_token token = ats_call_get_token( &f->call );
			uint32_t saved = current;
			current = f->index + 1;		// キャンセル中の再入を防ぐ
			cmd.cancel( this, token, cmd.user );
			current = saved;
		}
	}
	FinishFiber( f );
}

void ats_vm::FiberError( Fiber* f, const char* fmt, ... )
{
	char msg[ kErrorSize ];
	va_list ap;
	va_start( ap, fmt );
	FormatV( msg, sizeof(msg), fmt, ap );
	va_end( ap );

	const ats_program* prog = f->binding->prog;
	uint32_t line = prog->LineOf( f->pc );
	if( line ) Log( ATS_LOG_ERROR, "[%s:%u] %s", prog->script_id, line, msg );
	else       Log( ATS_LOG_ERROR, "[%s pc=%u] %s", prog->script_id, f->pc, msg );
}

//=========================================================================
// コマンド呼び出し
//=========================================================================
namespace {

enum class Flow { Continue, Suspend, Stop };

ats_value DefaultValue( uint32_t type )
{
	ats_value v;
	memset( &v, 0, sizeof(v) );
	v.type = type;
	if( type == ATS_TYPE_STRING ) v.v.s = "";
	return v;
}

// 完了した呼び出しの結果を積んで、呼び出しを閉じる
void FinishCall( ats_vm* vm, Fiber* f )
{
	const ImportEntry& ie = f->binding->prog->imports[ f->call.import ];
	if( ie.retc ){
		const ats_value& r = f->call.has_result ? f->call.result : DefaultValue( ie.ret_type );
		f->stack.Push( vm->ToSlot( r ) );
	}
	f->call.active = false;
	vm->ReleaseChannel( f );
}

const char* ImportName( Fiber* f )
{
	const ats_program* prog = f->binding->prog;
	return prog->strings[ prog->imports[ f->call.import ].name ];
}

// ハンドラの戻り値を処理する
Flow HandleStatus( ats_vm* vm, Fiber* f, ats_status st )
{
	// ハンドラ実行中に完了・失敗が通知されていれば、それを優先する
	if( f->call.outcome == 1 ) st = ATS_DONE;
	else if( f->call.outcome == 2 ) st = ATS_FAIL;
	f->call.outcome = 0;

	switch( st ){
	case ATS_DONE:
		FinishCall( vm, f );
		f->state = FiberState::Ready;
		break;
	case ATS_PENDING:
		if( !f->call.token_seq ) ats_call_get_token( &f->call );
		f->state = FiberState::WaitCall;
		break;
	case ATS_RUNNING:
		f->state = FiberState::CallRunning;
		break;
	case ATS_FAIL:
	default:
		f->state = FiberState::Ready;
		vm->FiberError( f, st == ATS_FAIL ? "command '%s' failed" : "command '%s' returned an invalid status", ImportName( f ) );
		f->call.active = false;
		vm->ReleaseChannel( f );
		vm->current = 0;
		vm->AbortFiber( f, false );
		return Flow::Stop;
	}

	if( f->abort_requested ){
		vm->current = 0;
		vm->AbortFiber( f, true );
		return Flow::Stop;
	}
	return f->state == FiberState::Ready ? Flow::Continue : Flow::Suspend;
}

// 呼び出しの準備（引数をスタックから取り出す）
void PrepareCall( ats_vm* vm, Fiber* f, uint32_t imp, uint32_t argc )
{
	const ImportEntry& ie = f->binding->prog->imports[ imp ];
	ats_call& c = f->call;
	c.active     = true;
	c.import     = imp;
	c.token_seq  = 0;
	c.retry      = 0;
	c.has_result = false;
	c.outcome    = 0;
	c.args.Resize( argc );
	uint32_t base = f->stack.Size() - argc;
	for( uint32_t i = 0; i < argc; ++i ) c.args[i] = vm->FromSlot( f->stack[ base + i ], ie.param_types[i] );
	f->stack.Resize( base );
}

Flow ExecCommand( ats_vm* vm, Fiber* f, uint32_t imp, uint32_t argc )
{
	const ats_program* prog = f->binding->prog;
	const CommandDef& cmd = vm->rt->commands[ prog->resolved[ imp ].index ];

	// チャンネルが使用中なら空くまで待つ（pc はこの命令のまま）
	if( cmd.channel != kNone ){
		Channel* ch = vm->channels[ cmd.channel ];
		if( ch->owner && ch->owner != f->index + 1 ){
			if( !ch->waiters.Push( vm->FiberId( f ) ) ){
				vm->FiberError( f, "out of memory (channel)" );
				vm->current = 0;
				vm->AbortFiber( f, false );
				return Flow::Stop;
			}
			f->state = FiberState::WaitChannel;
			return Flow::Suspend;
		}
	}

	f->pc += 1 + 3;
	PrepareCall( vm, f, imp, argc );
	if( cmd.channel != kNone ){
		vm->channels[ cmd.channel ]->owner = f->index + 1;
		f->call.holds_channel = true;
	}
	ats_status st = cmd.fn( &f->call, cmd.user );
	return HandleStatus( vm, f, st );
}

Flow ExecQuery( ats_vm* vm, Fiber* f, uint32_t imp, uint32_t argc )
{
	const ats_program* prog = f->binding->prog;
	const QueryDef& q = vm->rt->queries[ prog->resolved[ imp ].index ];

	f->pc += 1 + 3;
	PrepareCall( vm, f, imp, argc );
	ats_result r = q.fn( &f->call, q.user );
	if( r != ATS_OK ){
		f->call.active = false;
		vm->FiberError( f, "query '%s' failed (%s)", ImportName( f ), ats_result_string( r ) );
		vm->current = 0;
		vm->AbortFiber( f, false );
		return Flow::Stop;
	}
	FinishCall( vm, f );
	if( f->abort_requested ){
		vm->current = 0;
		vm->AbortFiber( f, true );
		return Flow::Stop;
	}
	return Flow::Continue;
}

}	// namespace

bool ats_vm::ResumeRunningCall( Fiber* f )
{
	const ats_program* prog = f->binding->prog;
	const CommandDef& cmd = rt->commands[ prog->resolved[ f->call.import ].index ];
	++f->call.retry;
	current = f->index + 1;
	ats_status st = cmd.fn( &f->call, cmd.user );
	Flow fl = HandleStatus( this, f, st );
	current = 0;
	return fl == Flow::Continue;
}

//=========================================================================
// 命令実行
//=========================================================================
void ats_vm::RunFiber( Fiber* f, uint32_t& remaining )
{
	current = f->index + 1;

	Binding* b = f->binding;
	const ats_program* prog = b->prog;
	const uint8_t* code = prog->code;
	Array<uint64_t>& st = f->stack;

#define ATS_FAIL_FIBER( ... )	do { FiberError( f, __VA_ARGS__ ); current = 0; AbortFiber( f, false ); return; } while( 0 )
#define ATS_POP()				( st.Pop(), st.Data()[ st.Size() ] )
#define ATS_TOP()				( st.Back() )

	while( remaining ){
		--remaining;
		const uint32_t pc = f->pc;
		const uint8_t  op = code[ pc ];
		const uint8_t* a  = code + pc + 1;
		uint32_t next = pc + 1 + (uint32_t)OperandSize( op );
		Frame& fr = f->callstack.Back();
		uint64_t* locals = st.Data() + fr.locals_base;

		switch( op ){
		case OP_NOP: break;

		// 定数・スタック ---------------------------------------
		case OP_PUSH_I32:	st.Push( FromI( Read<int32_t>( a ) ) ); break;
		case OP_PUSH_F32:	st.Push( Read<uint32_t>( a ) ); break;
		case OP_PUSH_K:		st.Push( prog->consts[ Read<uint32_t>( a ) ] ); break;
		case OP_PUSH_STR:	st.Push( b->str_ids[ Read<uint32_t>( a ) ] ); break;
		case OP_POP:		st.Pop(); break;
		case OP_DUP:		st.Push( ATS_TOP() ); break;

		// ローカル・変数 ---------------------------------------
		case OP_LD_LOCAL:	st.Push( locals[ Read<uint16_t>( a ) ] ); break;
		case OP_ST_LOCAL:	locals[ Read<uint16_t>( a ) ] = ATS_POP(); break;
		case OP_LD_VAR: {
			const VarRef& r = b->vars[ Read<uint16_t>( a ) ];
			st.Push( r.script ? b->block->vars[ r.index ].value : shared[ r.index ] );
			break;
		}
		case OP_ST_VAR: {
			const VarRef& r = b->vars[ Read<uint16_t>( a ) ];
			uint64_t v = ATS_POP();
			if( r.script ) b->block->vars[ r.index ].value = v;
			else           shared[ r.index ] = v;
			break;
		}

		// 整数演算（オーバーフローは 2 の補数で折り返す） -------
		case OP_ADD_I: { uint32_t y = (uint32_t)ATS_POP(); uint32_t x = (uint32_t)ATS_POP(); st.Push( (uint64_t)(uint32_t)(x + y) ); break; }
		case OP_SUB_I: { uint32_t y = (uint32_t)ATS_POP(); uint32_t x = (uint32_t)ATS_POP(); st.Push( (uint64_t)(uint32_t)(x - y) ); break; }
		case OP_MUL_I: { uint32_t y = (uint32_t)ATS_POP(); uint32_t x = (uint32_t)ATS_POP(); st.Push( (uint64_t)(uint32_t)(x * y) ); break; }
		case OP_DIV_I:
		case OP_MOD_I: {
			int32_t y = SlotI( ATS_POP() ); int32_t x = SlotI( ATS_POP() );
			if( y == 0 ) ATS_FAIL_FIBER( "division by zero" );
			int32_t r;
			if( x == INT32_MIN && y == -1 ) r = (op == OP_DIV_I) ? INT32_MIN : 0;
			else r = (op == OP_DIV_I) ? x / y : x % y;
			st.Push( FromI( r ) );
			break;
		}
		case OP_NEG_I: { uint32_t x = (uint32_t)ATS_POP(); st.Push( (uint64_t)(uint32_t)(0u - x) ); break; }

		// 実数演算 ---------------------------------------------
		case OP_ADD_F: { float y = SlotF( ATS_POP() ); float x = SlotF( ATS_POP() ); st.Push( FromF( x + y ) ); break; }
		case OP_SUB_F: { float y = SlotF( ATS_POP() ); float x = SlotF( ATS_POP() ); st.Push( FromF( x - y ) ); break; }
		case OP_MUL_F: { float y = SlotF( ATS_POP() ); float x = SlotF( ATS_POP() ); st.Push( FromF( x * y ) ); break; }
		case OP_DIV_F: { float y = SlotF( ATS_POP() ); float x = SlotF( ATS_POP() ); st.Push( FromF( x / y ) ); break; }
		case OP_NEG_F: { float x = SlotF( ATS_POP() ); st.Push( FromF( -x ) ); break; }

		// ビット演算・論理否定 ---------------------------------
		case OP_BAND: { uint32_t y = (uint32_t)ATS_POP(); uint32_t x = (uint32_t)ATS_POP(); st.Push( x & y ); break; }
		case OP_BOR:  { uint32_t y = (uint32_t)ATS_POP(); uint32_t x = (uint32_t)ATS_POP(); st.Push( x | y ); break; }
		case OP_BXOR: { uint32_t y = (uint32_t)ATS_POP(); uint32_t x = (uint32_t)ATS_POP(); st.Push( x ^ y ); break; }
		case OP_SHL:  { uint32_t y = (uint32_t)ATS_POP(); uint32_t x = (uint32_t)ATS_POP(); st.Push( (uint64_t)(uint32_t)(x << (y & 31)) ); break; }
		case OP_SHR:  { uint32_t y = (uint32_t)ATS_POP(); int32_t x = SlotI( ATS_POP() ); st.Push( FromI( x >> (y & 31) ) ); break; }
		case OP_BNOT: { uint32_t x = (uint32_t)ATS_POP(); st.Push( (uint64_t)(uint32_t)~x ); break; }
		case OP_NOT:  { uint64_t x = ATS_POP(); st.Push( x ? 0 : 1 ); break; }

		// 比較 -------------------------------------------------
#define ATS_CMP_I( OPC, EXPR ) case OPC: { int32_t y = SlotI( ATS_POP() ); int32_t x = SlotI( ATS_POP() ); st.Push( (EXPR) ? 1 : 0 ); break; }
#define ATS_CMP_F( OPC, EXPR ) case OPC: { float y = SlotF( ATS_POP() ); float x = SlotF( ATS_POP() ); st.Push( (EXPR) ? 1 : 0 ); break; }
		ATS_CMP_I( OP_EQ_I, x == y ) ATS_CMP_I( OP_NE_I, x != y ) ATS_CMP_I( OP_LT_I, x < y )
		ATS_CMP_I( OP_LE_I, x <= y ) ATS_CMP_I( OP_GT_I, x > y )  ATS_CMP_I( OP_GE_I, x >= y )
		ATS_CMP_F( OP_EQ_F, x == y ) ATS_CMP_F( OP_NE_F, x != y ) ATS_CMP_F( OP_LT_F, x < y )
		ATS_CMP_F( OP_LE_F, x <= y ) ATS_CMP_F( OP_GT_F, x > y )  ATS_CMP_F( OP_GE_F, x >= y )
#undef ATS_CMP_I
#undef ATS_CMP_F
		case OP_EQ_H: { uint64_t y = ATS_POP(); uint64_t x = ATS_POP(); st.Push( x == y ? 1 : 0 ); break; }
		case OP_NE_H: { uint64_t y = ATS_POP(); uint64_t x = ATS_POP(); st.Push( x != y ? 1 : 0 ); break; }

		// 変換 -------------------------------------------------
		case OP_I2F: { int32_t x = SlotI( ATS_POP() ); st.Push( FromF( (float)x ) ); break; }
		case OP_F2I: {
			float x = SlotF( ATS_POP() );
			int32_t r;
			if( !(x == x) ) r = 0;
			else if( x >= 2147483647.0f ) r = INT32_MAX;
			else if( x <= -2147483648.0f ) r = INT32_MIN;
			else r = (int32_t)x;
			st.Push( FromI( r ) );
			break;
		}

		// 分岐 -------------------------------------------------
		case OP_JMP:	next += (uint32_t)Read<int32_t>( a ); break;
		case OP_JZ:		if( ATS_POP() == 0 ) next += (uint32_t)Read<int32_t>( a ); break;
		case OP_JNZ:	if( ATS_POP() != 0 ) next += (uint32_t)Read<int32_t>( a ); break;
		case OP_SWITCH: {
			uint16_t count = Read<uint16_t>( a );
			int32_t  def   = Read<int32_t>( a + 2 );
			int32_t  v     = SlotI( ATS_POP() );
			next += (uint32_t)count * 8;
			int32_t  off   = def;
			for( uint32_t k = 0; k < count; ++k ){
				if( Read<int32_t>( a + 6 + k * 8 ) == v ){ off = Read<int32_t>( a + 6 + k * 8 + 4 ); break; }
			}
			next += (uint32_t)off;
			break;
		}

		// 呼び出し ---------------------------------------------
		case OP_CALL_CMD:
		case OP_CALL_QUERY: {
			uint16_t imp  = Read<uint16_t>( a );
			uint8_t  argc = a[2];
			Flow fl = (op == OP_CALL_CMD) ? ExecCommand( this, f, imp, argc ) : ExecQuery( this, f, imp, argc );
			if( fl == Flow::Stop ) return;
			if( fl == Flow::Suspend ){ current = 0; return; }
			continue;	// pc は ExecCommand / ExecQuery が進めた
		}
		case OP_CALL_FN: {
			uint16_t fn = Read<uint16_t>( a );
			const EntryEntry& e = prog->entries[ fn ];
			if( f->callstack.Size() >= max_depth ) ATS_FAIL_FIBER( "call depth exceeded" );
			uint32_t base = st.Size() - e.paramc;
			if( base + e.localc + e.max_stack > max_stack ) ATS_FAIL_FIBER( "stack overflow" );
			st.Resize( base + e.localc );
			for( uint32_t k = base + e.paramc; k < base + e.localc; ++k ) st[k] = 0;
			Frame nf;
			nf.entry       = fn;
			nf.ret_pc      = next;
			nf.locals_base = base;
			f->callstack.Push( nf );
			next = e.code;
			break;
		}
		case OP_RET: {
			const EntryEntry& e = prog->entries[ fr.entry ];
			uint64_t rv = e.retc ? ATS_TOP() : 0;
			uint32_t base = fr.locals_base;
			uint32_t ret  = fr.ret_pc;
			f->callstack.Pop();
			if( f->callstack.Empty() || ret == kNoReturn ){
				current = 0;
				FinishFiber( f );
				return;
			}
			st.Resize( base );
			if( e.retc ) st.Push( rv );
			next = ret;
			break;
		}

		// 並行・待機 -------------------------------------------
		case OP_FORK: {
			const EntryEntry& e = prog->entries[ fr.entry ];
			Fiber* c = NewFiber( b, fr.entry );
			if( !c ) ATS_FAIL_FIBER( "failed to fork" );
			// 子は現在のローカルのコピーを持ち、空の演算スタックで始まる
			memcpy( c->stack.Data(), locals, sizeof(uint64_t) * e.localc );
			c->pc         = next + (uint32_t)Read<int32_t>( a );
			c->parent     = f->index + 1;
			c->parent_gen = f->generation;
			if( !f->children.Push( c->index ) ){ AbortFiber( c, false ); ATS_FAIL_FIBER( "out of memory (fork)" ); }
			++f->live_children;
			break;
		}
		case OP_JOIN:
		case OP_RACE:
			if( op == OP_RACE && f->finished_children > 0 ){
				while( !f->children.Empty() ){
					Fiber* c = fibers[ f->children.Back() ];
					AbortFiber( c, true );
				}
			}
			if( f->live_children == 0 ){
				f->finished_children = 0;
				break;
			}
			f->join_mode = op;
			f->state     = FiberState::WaitJoin;
			f->pc        = next;
			current = 0;
			return;
		case OP_YIELD:
			f->frames = 1;
			f->state  = FiberState::WaitFrames;
			f->pc     = next;
			current = 0;
			return;
		case OP_SLEEP: {
			float t = SlotF( ATS_POP() );
			if( !(t > 0.0f) ) break;
			f->sleep = t;
			f->state = FiberState::Sleeping;
			f->pc    = next;
			current = 0;
			return;
		}
		case OP_WAIT_FRAMES: {
			int32_t n = SlotI( ATS_POP() );
			if( n <= 0 ) break;
			f->frames = (uint32_t)n;
			f->state  = FiberState::WaitFrames;
			f->pc     = next;
			current = 0;
			return;
		}
		case OP_END:
			current = 0;
			FinishFiber( f );
			return;

		// その他 -----------------------------------------------
		case OP_FIRE: {
			const char* name = prog->strings[ Read<uint32_t>( a ) ];
			uint8_t argc = a[4];
			uint32_t base = st.Size() - argc;
			for( uint32_t i = 0; i < bindings.Size(); ++i ){
				Binding* tb = bindings[i];
				int entry = tb->prog->FindEntry( name, kEntryEvent );
				if( entry < 0 ) continue;
				const EntryEntry& te = tb->prog->entries[ (uint32_t)entry ];
				if( te.paramc != argc ){
					Log( ATS_LOG_WARNING, "[%s] fire '%s': argument count mismatch with '%s'", prog->script_id, name, tb->prog->script_id );
					continue;
				}
				Fiber* c = NewFiber( tb, (uint32_t)entry );
				if( !c ) continue;
				memcpy( c->stack.Data(), st.Data() + base, sizeof(uint64_t) * argc );
			}
			st.Resize( base );
			break;
		}
		case OP_RESET_VARS:
			ResetBlock( b->block );
			break;
		case OP_RAND: {
			int32_t n = SlotI( ATS_POP() );
			st.Push( FromI( n > 0 ? (int32_t)(NextRandom( rng ) % (uint64_t)n) : 0 ) );
			break;
		}

		default:
			ATS_FAIL_FIBER( "bad opcode %u", op );
		}
		f->pc = next;
	}

	// 命令数の上限に達した（次の update で続きから）
	current = 0;

#undef ATS_FAIL_FIBER
#undef ATS_POP
#undef ATS_TOP
}

//=========================================================================
// 公開関数：VM
//=========================================================================
extern "C" ATS_API ats_result ATS_CALL ats_vm_create( ats_runtime* rt, const ats_vm_desc* desc, ats_vm** out )
{
	if( !rt || !out ) return ATS_ERR_INVALID_ARG;
	*out = nullptr;
	if( desc && desc->size < sizeof(ats_vm_desc) ) return ATS_ERR_INVALID_ARG;

	ats_vm* vm = rt->alloc.New<ats_vm>();
	if( !vm ) return ATS_ERR_OUT_OF_MEMORY;
	vm->rt        = rt;
	vm->alloc     = &rt->alloc;
	vm->budget    = (desc && desc->instruction_budget) ? desc->instruction_budget : kDefaultBudget;
	vm->max_stack = (desc && desc->max_stack) ? desc->max_stack : kDefaultMaxStack;
	vm->max_depth = (desc && desc->max_call_depth) ? desc->max_call_depth : kDefaultMaxDepth;
	vm->rng       = (desc && desc->random_seed) ? desc->random_seed : 0x2545F4914F6CDD1Dull;
	vm->in_update = false;
	vm->current   = 0;
	vm->token_seq = 0;
	vm->error[0]  = '\0';
	vm->shared.Init( vm->alloc );
	vm->blocks.Init( vm->alloc );
	vm->bindings.Init( vm->alloc );
	vm->fibers.Init( vm->alloc );
	vm->channels.Init( vm->alloc );
	vm->interned.Init( vm->alloc );
	vm->intern_map.Init( vm->alloc );

	// 文字列 ID 0 は空文字列
	vm->Intern( "" );

	// 共有変数の初期値
	if( !vm->shared.Resize( rt->vars.Size() ) ){ ats_vm_destroy( vm ); return ATS_ERR_OUT_OF_MEMORY; }
	for( uint32_t i = 0; i < rt->vars.Size(); ++i ){
		const VarDef& d = rt->vars[i];
		vm->shared[i] = (d.type == ATS_TYPE_STRING) ? vm->Intern( d.init_str ? d.init_str : "" ) : d.init;
	}

	// チャンネル
	for( uint32_t i = 0; i < rt->channels.Size(); ++i ){
		Channel* ch = vm->alloc->New<Channel>();
		if( !ch || !vm->channels.Push( ch ) ){ vm->alloc->Delete( ch ); ats_vm_destroy( vm ); return ATS_ERR_OUT_OF_MEMORY; }
		ch->owner = 0;
		ch->waiters.Init( vm->alloc );
	}

	*out = vm;
	return ATS_OK;
}

extern "C" ATS_API void ATS_CALL ats_vm_destroy( ats_vm* vm )
{
	if( !vm ) return;
	const Allocator* a = vm->alloc;

	// 実行中のファイバを中断する（待機中のコマンドにはキャンセルを通知）
	vm->current = 0;
	for( uint32_t i = 0; i < vm->fibers.Size(); ++i ){
		Fiber* f = vm->fibers[i];
		if( f->state != FiberState::Free ) vm->AbortFiber( f, true );
	}
	for( uint32_t i = 0; i < vm->fibers.Size(); ++i ) a->Delete( vm->fibers[i] );

	for( uint32_t i = 0; i < vm->bindings.Size(); ++i ){
		Binding* b = vm->bindings[i];
		ats_program_release( b->prog );
		a->Delete( b );
	}
	for( uint32_t i = 0; i < vm->blocks.Size(); ++i ){
		ScriptBlock* blk = vm->blocks[i];
		for( uint32_t k = 0; k < blk->vars.Size(); ++k ){ a->Free( blk->vars[k].name ); a->Free( blk->vars[k].was_name ); }
		for( uint32_t k = 0; k < blk->dormant.Size(); ++k ){ a->Free( blk->dormant[k].name ); a->Free( blk->dormant[k].str ); }
		a->Free( blk->script_id );
		a->Delete( blk );
	}
	for( uint32_t i = 0; i < vm->channels.Size(); ++i ) a->Delete( vm->channels[i] );
	for( uint32_t i = 0; i < vm->interned.Size(); ++i ) a->Free( vm->interned[i] );

	vm->~ats_vm();
	a->Free( vm );
}

extern "C" ATS_API const char* ATS_CALL ats_vm_last_error( ats_vm* vm )
{
	return vm ? vm->error : "";
}

extern "C" ATS_API ats_result ATS_CALL ats_vm_attach( ats_vm* vm, ats_program* prog )
{
	if( !vm || !prog || prog->rt != vm->rt ) return ATS_ERR_INVALID_ARG;
	return vm->Attach( prog, nullptr );
}

extern "C" ATS_API ats_result ATS_CALL ats_vm_detach( ats_vm* vm, ats_program* prog )
{
	if( !vm || !prog ) return ATS_ERR_INVALID_ARG;
	if( vm->in_update ) return ATS_ERR_BUSY;
	for( uint32_t i = 0; i < vm->bindings.Size(); ++i ){
		Binding* b = vm->bindings[i];
		if( b->prog != prog ) continue;
		for( uint32_t k = 0; k < vm->fibers.Size(); ++k ){
			Fiber* f = vm->fibers[k];
			if( f->state != FiberState::Free && f->binding == b ) vm->AbortFiber( f, true );
		}
		vm->bindings.RemoveAt( i );
		ats_program_release( prog );
		vm->alloc->Delete( b );
		return ATS_OK;
	}
	return ATS_ERR_NOT_FOUND;
}

extern "C" ATS_API ats_result ATS_CALL ats_vm_update( ats_vm* vm, float dt )
{
	if( !vm ) return ATS_ERR_INVALID_ARG;
	if( vm->in_update ) return ATS_ERR_BUSY;
	vm->in_update = true;

	// 待機時間を進める
	for( uint32_t i = 0; i < vm->fibers.Size(); ++i ){
		Fiber* f = vm->fibers[i];
		if( f->state == FiberState::Sleeping ){
			f->sleep -= dt;
			if( f->sleep <= 0.0f ) f->state = FiberState::Ready;
		} else if( f->state == FiberState::WaitFrames ){
			if( --f->frames == 0 ) f->state = FiberState::Ready;
		}
	}

	// ATS_RUNNING のハンドラを呼び直す
	for( uint32_t i = 0; i < vm->fibers.Size(); ++i ){
		Fiber* f = vm->fibers[i];
		if( f->state == FiberState::CallRunning ) vm->ResumeRunningCall( f );
	}

	// 実行できるファイバを順に動かす（実行中に生まれたファイバも同じ update で動かす）
	uint32_t budget = vm->budget;
	bool progress = true;
	while( progress && budget ){
		progress = false;
		for( uint32_t i = 0; i < vm->fibers.Size() && budget; ++i ){
			Fiber* f = vm->fibers[i];
			if( f->state != FiberState::Ready ) continue;
			vm->RunFiber( f, budget );
			progress = true;
		}
	}
	if( !budget ){
		for( uint32_t i = 0; i < vm->fibers.Size(); ++i ){
			if( vm->fibers[i]->state == FiberState::Ready ){
				vm->Log( ATS_LOG_WARNING, "instruction budget (%u) exceeded; remaining fibers continue next update", vm->budget );
				break;
			}
		}
	}

	vm->in_update = false;
	return ATS_OK;
}

extern "C" ATS_API ats_result ATS_CALL ats_vm_fire_event( ats_vm* vm, ats_program* prog, const char* event,
														 const ats_value* args, int argc, ats_fiber_id* out )
{
	if( out ) *out = ATS_INVALID_ID;
	if( !vm || !prog || !event || argc < 0 || (argc && !args) ) return ATS_ERR_INVALID_ARG;
	if( prog->rt != vm->rt ) return ATS_ERR_INVALID_ARG;
	Binding* b;
	ats_result r = vm->Attach( prog, &b );
	if( r != ATS_OK ) return r;
	return vm->StartEvent( b, event, args, argc, out );
}

extern "C" ATS_API ats_result ATS_CALL ats_vm_broadcast_event( ats_vm* vm, const char* event,
															  const ats_value* args, int argc, int* out_count )
{
	if( out_count ) *out_count = 0;
	if( !vm || !event || argc < 0 || (argc && !args) ) return ATS_ERR_INVALID_ARG;
	int count = 0;
	for( uint32_t i = 0; i < vm->bindings.Size(); ++i ){
		ats_result r = vm->StartEvent( vm->bindings[i], event, args, argc, nullptr );
		if( r == ATS_OK ) ++count;
		else if( r != ATS_ERR_NOT_FOUND ) return r;
	}
	if( out_count ) *out_count = count;
	return ATS_OK;
}

extern "C" ATS_API ats_result ATS_CALL ats_vm_abort_fiber( ats_vm* vm, ats_fiber_id fiber )
{
	if( !vm ) return ATS_ERR_INVALID_ARG;
	Fiber* f = vm->GetFiber( fiber );
	if( !f ) return ATS_ERR_STALE_ID;
	vm->AbortFiber( f, true );
	return ATS_OK;
}

extern "C" ATS_API int ATS_CALL ats_vm_is_fiber_alive( ats_vm* vm, ats_fiber_id fiber )
{
	if( !vm ) return 0;
	Fiber* f = vm->GetFiber( fiber );
	return f && f->state != FiberState::Dying ? 1 : 0;
}

extern "C" ATS_API int ATS_CALL ats_vm_fiber_count( ats_vm* vm )
{
	if( !vm ) return 0;
	int n = 0;
	for( uint32_t i = 0; i < vm->fibers.Size(); ++i )
		if( vm->fibers[i]->state != FiberState::Free ) ++n;
	return n;
}

//=========================================================================
// 公開関数：ハンドラから使う関数
//=========================================================================
extern "C" ATS_API int ATS_CALL ats_call_argc( ats_call* call )
{
	return call ? (int)call->args.Size() : 0;
}

extern "C" ATS_API ats_value ATS_CALL ats_call_arg( ats_call* call, int index )
{
	if( !call || index < 0 || (uint32_t)index >= call->args.Size() ) return DefaultValue( ATS_TYPE_VOID );
	return call->args[ (uint32_t)index ];
}

extern "C" ATS_API int32_t ATS_CALL ats_arg_int( ats_call* call, int index )
{
	ats_value v = ats_call_arg( call, index );
	switch( v.type ){
	case ATS_TYPE_INT: case ATS_TYPE_ENUM:	return v.v.i;
	case ATS_TYPE_BOOL:						return v.v.b;
	case ATS_TYPE_FLOAT:					return (int32_t)v.v.f;
	}
	return 0;
}

extern "C" ATS_API float ATS_CALL ats_arg_float( ats_call* call, int index )
{
	ats_value v = ats_call_arg( call, index );
	switch( v.type ){
	case ATS_TYPE_FLOAT:					return v.v.f;
	case ATS_TYPE_INT: case ATS_TYPE_ENUM:	return (float)v.v.i;
	case ATS_TYPE_BOOL:						return (float)v.v.b;
	}
	return 0.0f;
}

extern "C" ATS_API int ATS_CALL ats_arg_bool( ats_call* call, int index )
{
	return ats_arg_int( call, index ) != 0 ? 1 : 0;
}

extern "C" ATS_API const char* ATS_CALL ats_arg_string( ats_call* call, int index )
{
	ats_value v = ats_call_arg( call, index );
	return v.type == ATS_TYPE_STRING && v.v.s ? v.v.s : "";
}

extern "C" ATS_API uint64_t ATS_CALL ats_arg_handle( ats_call* call, int index )
{
	ats_value v = ats_call_arg( call, index );
	return v.type == ATS_TYPE_HANDLE ? v.v.h : 0;
}

extern "C" ATS_API ats_result ATS_CALL ats_call_set_result( ats_call* call, const ats_value* value )
{
	if( !call || !value || !call->active ) return ATS_ERR_INVALID_ARG;
	ats_vm* vm = call->vm;
	Fiber* f = vm->fibers[ call->fiber_index ];
	const ImportEntry& ie = f->binding->prog->imports[ call->import ];
	if( !ie.retc ) return ATS_ERR_INVALID_ARG;
	if( !TypeMatch( ie.ret_type, value->type ) ) return ATS_ERR_TYPE;
	call->result = *value;
	call->result.type = ie.ret_type;
	// 文字列はホスト側のバッファかもしれないので VM 内にコピーする
	if( value->type == ATS_TYPE_STRING ) call->result.v.s = vm->Str( vm->Intern( value->v.s ) );
	call->has_result = true;
	return ATS_OK;
}

extern "C" ATS_API ats_call_token ATS_CALL ats_call_get_token( ats_call* call )
{
	if( !call || !call->active ) return ATS_INVALID_ID;
	if( !call->token_seq ){
		if( ++call->vm->token_seq == 0 ) call->vm->token_seq = 1;
		call->token_seq = call->vm->token_seq;
	}
	return ((uint64_t)call->token_seq << 32) | (uint64_t)(call->fiber_index + 1);
}

extern "C" ATS_API ats_fiber_id ATS_CALL ats_call_fiber( ats_call* call )
{
	if( !call ) return ATS_INVALID_ID;
	return call->vm->FiberId( call->vm->fibers[ call->fiber_index ] );
}

extern "C" ATS_API ats_vm* ATS_CALL ats_call_vm( ats_call* call )
{
	return call ? call->vm : nullptr;
}

extern "C" ATS_API uint32_t ATS_CALL ats_call_retry_count( ats_call* call )
{
	return call ? call->retry : 0;
}

//=========================================================================
// 公開関数：待機中の呼び出しの完了
//=========================================================================
static Fiber* FiberFromToken( ats_vm* vm, ats_call_token token )
{
	uint32_t idx = (uint32_t)token;
	uint32_t seq = (uint32_t)(token >> 32);
	if( !vm || idx == 0 || idx > vm->fibers.Size() || seq == 0 ) return nullptr;
	Fiber* f = vm->fibers[ idx - 1 ];
	if( f->state == FiberState::Free || f->state == FiberState::Dying ) return nullptr;
	if( !f->call.active || f->call.token_seq != seq ) return nullptr;
	return f;
}

extern "C" ATS_API ats_result ATS_CALL ats_call_complete( ats_vm* vm, ats_call_token token, const ats_value* result )
{
	Fiber* f = FiberFromToken( vm, token );
	if( !f ) return ATS_ERR_STALE_ID;
	if( result ){
		ats_result r = ats_call_set_result( &f->call, result );
		if( r != ATS_OK ) return r;
	}
	// ハンドラの実行中なら、戻り値の処理で反映する
	if( vm->current == f->index + 1 ){
		f->call.outcome = 1;
		return ATS_OK;
	}
	if( f->state != FiberState::WaitCall && f->state != FiberState::CallRunning ) return ATS_ERR_BUSY;
	FinishCall( vm, f );
	f->state = FiberState::Ready;
	return ATS_OK;
}

extern "C" ATS_API ats_result ATS_CALL ats_call_fail( ats_vm* vm, ats_call_token token, const char* reason )
{
	Fiber* f = FiberFromToken( vm, token );
	if( !f ) return ATS_ERR_STALE_ID;
	if( vm->current == f->index + 1 ){
		f->call.outcome = 2;
		return ATS_OK;
	}
	if( f->state != FiberState::WaitCall && f->state != FiberState::CallRunning ) return ATS_ERR_BUSY;
	vm->FiberError( f, "command '%s' failed: %s", ImportName( f ), reason ? reason : "" );
	f->call.active = false;
	vm->ReleaseChannel( f );
	vm->AbortFiber( f, false );
	return ATS_OK;
}
