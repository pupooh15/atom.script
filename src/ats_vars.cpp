/**************************************************************************/
/*!	\file	ats_vars.cpp
	\brief	変数（プログラムとの結び付け・スクリプト変数・セーブ／ロード）
	\note
	セーブに入るのは共有変数（persistent）・スクリプト変数・乱数の状態だけ。
	実行中のファイバは保存しない。再開位置はスクリプトが変数を見て判断する。
***************************************************************************/
#include "ats_internal.h"

using namespace ats;
using namespace ats::fmt;

//=========================================================================
// 型変換（セーブデータや型変更への対応）
//=========================================================================
bool ats_vm::ConvertSlot( uint64_t* slot, uint32_t from, uint32_t to, const char* fromStr )
{
	if( from == to || (IsIntLike( from ) && IsIntLike( to )) ){
		if( to == ATS_TYPE_STRING ) *slot = Intern( fromStr );
		return true;
	}
	switch( to ){
	case ATS_TYPE_INT:
	case ATS_TYPE_ENUM:
		if( from == ATS_TYPE_FLOAT ){ *slot = FromI( (int32_t)SlotF( *slot ) ); return true; }
		if( from == ATS_TYPE_BOOL ){ *slot = FromI( *slot ? 1 : 0 ); return true; }
		break;
	case ATS_TYPE_FLOAT:
		if( IsIntLike( from ) ){ *slot = FromF( (float)SlotI( *slot ) ); return true; }
		if( from == ATS_TYPE_BOOL ){ *slot = FromF( *slot ? 1.0f : 0.0f ); return true; }
		break;
	case ATS_TYPE_BOOL:
		if( IsIntLike( from ) ){ *slot = SlotI( *slot ) ? 1 : 0; return true; }
		break;
	}
	return false;
}

//=========================================================================
// スクリプトブロック
//=========================================================================
ScriptBlock* ats_vm::FindBlock( const char* script_id )
{
	for( uint32_t i = 0; i < blocks.Size(); ++i )
		if( strcmp( blocks[i]->script_id, script_id ) == 0 ) return blocks[i];
	return nullptr;
}

ScriptBlock* ats_vm::GetBlock( const char* script_id )
{
	ScriptBlock* blk = FindBlock( script_id );
	if( blk ) return blk;
	blk = alloc->New<ScriptBlock>();
	if( !blk ) return nullptr;
	blk->vars.Init( alloc );
	blk->dormant.Init( alloc );
	blk->script_id = alloc->StrDup( script_id );
	if( !blk->script_id || !blocks.Push( blk ) ){
		alloc->Free( blk->script_id );
		alloc->Delete( blk );
		return nullptr;
	}
	return blk;
}

void ats_vm::ResetBlock( ScriptBlock* blk )
{
	for( uint32_t i = 0; i < blk->vars.Size(); ++i ) blk->vars[i].value = blk->vars[i].init;
	for( uint32_t i = 0; i < blk->dormant.Size(); ++i ){
		alloc->Free( blk->dormant[i].name );
		alloc->Free( blk->dormant[i].str );
	}
	blk->dormant.Clear();
}

// 休眠中のセーブ値を探して取り出す（見つかれば dormant から外す）
static bool TakeDormant( ats_vm* vm, ScriptBlock* blk, const char* name, ScriptVar& sv )
{
	for( uint32_t i = 0; i < blk->dormant.Size(); ++i ){
		DormantVar& d = blk->dormant[i];
		if( strcmp( d.name, name ) != 0 ) continue;
		uint64_t v = d.value;
		if( vm->ConvertSlot( &v, d.type, sv.type, d.str ) ) sv.value = v;
		else vm->Log( ATS_LOG_WARNING, "[%s] saved variable '%s' has an incompatible type; using the initial value", blk->script_id, sv.name );
		vm->alloc->Free( d.name );
		vm->alloc->Free( d.str );
		blk->dormant.RemoveAt( i );
		return true;
	}
	return false;
}

//=========================================================================
// プログラムとの結び付け
//=========================================================================
Binding* ats_vm::FindBinding( ats_program* prog )
{
	for( uint32_t i = 0; i < bindings.Size(); ++i )
		if( bindings[i]->prog == prog ) return bindings[i];
	return nullptr;
}

ats_result ats_vm::Attach( ats_program* prog, Binding** out )
{
	Binding* b = FindBinding( prog );
	if( b ){ if( out ) *out = b; return ATS_OK; }

	b = alloc->New<Binding>();
	if( !b ) return ATS_ERR_OUT_OF_MEMORY;
	b->vars.Init( alloc );
	b->str_ids.Init( alloc );
	b->prog  = prog;
	b->block = GetBlock( prog->script_id );
	auto fail = [&]() { alloc->Delete( b ); return ATS_ERR_OUT_OF_MEMORY; };
	if( !b->block ) return fail();

	if( !b->str_ids.Resize( prog->strings.Size() ) ) return fail();
	for( uint32_t i = 0; i < prog->strings.Size(); ++i ) b->str_ids[i] = Intern( prog->strings[i] );

	if( !b->vars.Resize( prog->vars.Size() ) ) return fail();
	ScriptBlock* blk = b->block;
	for( uint32_t i = 0; i < prog->vars.Size(); ++i ){
		const VarEntry& v = prog->vars[i];
		if( v.kind == kVarShared ){
			b->vars[i].script = 0;
			b->vars[i].index  = (uint32_t)prog->var_shared[i];
			continue;
		}

		const char* name = prog->strings[ v.name ];
		const char* was  = v.was_name != kNone ? prog->strings[ v.was_name ] : nullptr;
		uint64_t init = (v.type == ATS_TYPE_STRING) ? Intern( prog->strings[ (uint32_t)v.init ] ) : v.init;

		uint32_t idx = 0xFFFFFFFFu;
		for( uint32_t k = 0; k < blk->vars.Size(); ++k )
			if( strcmp( blk->vars[k].name, name ) == 0 ){ idx = k; break; }

		if( idx != 0xFFFFFFFFu ){
			// 既に確保済み（別バージョンのプログラムなど）。値は引き継ぐ
			ScriptVar& sv = blk->vars[idx];
			if( sv.type != v.type ){
				uint64_t val = sv.value;
				if( ConvertSlot( &val, sv.type, v.type, sv.type == ATS_TYPE_STRING ? Str( sv.value ) : nullptr ) ) sv.value = val;
				else sv.value = init;
				sv.type = v.type;
			}
			sv.init  = init;
			sv.flags = v.flags;
		} else {
			ScriptVar sv;
			memset( &sv, 0, sizeof(sv) );
			sv.name     = alloc->StrDup( name );
			sv.was_name = was ? alloc->StrDup( was ) : nullptr;
			sv.type     = v.type;
			sv.flags    = v.flags;
			sv.init     = init;
			sv.value    = init;
			if( !sv.name || (was && !sv.was_name) || !blk->vars.Push( sv ) ){
				alloc->Free( sv.name );
				alloc->Free( sv.was_name );
				return fail();
			}
			idx = blk->vars.Size() - 1;
			// セーブ済みで未読み込みの値があれば適用する（なければ旧名でも探す）
			ScriptVar& nv = blk->vars[idx];
			if( !TakeDormant( this, blk, name, nv ) && was ) TakeDormant( this, blk, was, nv );
		}
		b->vars[i].script = 1;
		b->vars[i].index  = idx;
	}

	if( !bindings.Push( b ) ) return fail();
	++prog->refcount;
	if( out ) *out = b;
	return ATS_OK;
}

//=========================================================================
// 公開関数：共有変数・スクリプト変数
//=========================================================================
extern "C" ATS_API ats_result ATS_CALL ats_var_get( ats_vm* vm, uint32_t var_id, ats_value* out )
{
	if( !vm || !out ) return ATS_ERR_INVALID_ARG;
	int idx = vm->rt->FindVar( var_id );
	if( idx < 0 ) return ATS_ERR_NOT_FOUND;
	*out = vm->FromSlot( vm->shared[ (uint32_t)idx ], vm->rt->vars[ (uint32_t)idx ].type );
	return ATS_OK;
}

extern "C" ATS_API ats_result ATS_CALL ats_var_set( ats_vm* vm, uint32_t var_id, const ats_value* value )
{
	if( !vm || !value ) return ATS_ERR_INVALID_ARG;
	int idx = vm->rt->FindVar( var_id );
	if( idx < 0 ) return ATS_ERR_NOT_FOUND;
	uint32_t type = vm->rt->vars[ (uint32_t)idx ].type;
	if( value->type != type && !(IsIntLike( value->type ) && IsIntLike( type )) ) return ATS_ERR_TYPE;
	vm->shared[ (uint32_t)idx ] = vm->ToSlot( *value );
	return ATS_OK;
}

extern "C" ATS_API ats_result ATS_CALL ats_script_reset( ats_vm* vm, const char* script_id )
{
	if( !vm || !script_id ) return ATS_ERR_INVALID_ARG;
	ScriptBlock* blk = vm->FindBlock( script_id );
	if( blk ) vm->ResetBlock( blk );
	return ATS_OK;
}

#if defined(ATS_ENABLE_DEBUG)
static ScriptVar* FindScriptVar( ats_vm* vm, const char* script_id, const char* name )
{
	ScriptBlock* blk = vm->FindBlock( script_id );
	if( !blk ) return nullptr;
	for( uint32_t i = 0; i < blk->vars.Size(); ++i )
		if( strcmp( blk->vars[i].name, name ) == 0 ) return &blk->vars[i];
	return nullptr;
}

extern "C" ATS_API ats_result ATS_CALL ats_debug_script_var_get( ats_vm* vm, const char* script_id, const char* name, ats_value* out )
{
	if( !vm || !script_id || !name || !out ) return ATS_ERR_INVALID_ARG;
	ScriptVar* sv = FindScriptVar( vm, script_id, name );
	if( !sv ) return ATS_ERR_NOT_FOUND;
	*out = vm->FromSlot( sv->value, sv->type );
	return ATS_OK;
}

extern "C" ATS_API ats_result ATS_CALL ats_debug_script_var_set( ats_vm* vm, const char* script_id, const char* name, const ats_value* value )
{
	if( !vm || !script_id || !name || !value ) return ATS_ERR_INVALID_ARG;
	ScriptVar* sv = FindScriptVar( vm, script_id, name );
	if( !sv ) return ATS_ERR_NOT_FOUND;
	if( value->type != sv->type && !(IsIntLike( value->type ) && IsIntLike( sv->type )) ) return ATS_ERR_TYPE;
	sv->value = vm->ToSlot( *value );
	return ATS_OK;
}
#endif

//=========================================================================
// セーブ
//	u32 'ATSS', u16 major, u16 minor, u64 乱数状態
//	u32 共有変数の数, { u32 id, u8 type, 値 }
//	u32 スクリプト数, { 文字列 script_id, u32 変数の数, { 文字列 name, u8 type, 値 } }
//	値：STRING なら文字列、それ以外は u64。文字列：u32 長さ + バイト列
//=========================================================================
namespace {

constexpr uint32_t kSaveMagic		= FourCC( 'A', 'T', 'S', 'S' );
constexpr uint16_t kSaveMajor		= 1;
constexpr uint16_t kSaveMinor		= 0;
constexpr uint32_t kMaxSaveString	= 1u << 20;

struct Writer {
	ats_write_fn	fn;
	void*			user;
	ats_result		r = ATS_OK;

	void Bytes( const void* p, size_t n ) { if( r == ATS_OK && n ) r = fn( p, n, user ); }
	void U8( uint8_t v ) { Bytes( &v, 1 ); }
	void U16( uint16_t v ) { Bytes( &v, 2 ); }
	void U32( uint32_t v ) { Bytes( &v, 4 ); }
	void U64( uint64_t v ) { Bytes( &v, 8 ); }
	void Str( const char* s ) { uint32_t n = (uint32_t)strlen( s ); U32( n ); Bytes( s, n ); }
	void Value( ats_vm* vm, uint32_t type, uint64_t v, const char* str )
	{
		U8( (uint8_t)type );
		if( type == ATS_TYPE_STRING ) Str( str ? str : vm->Str( v ) );
		else U64( v );
	}
};

}	// namespace

extern "C" ATS_API ats_result ATS_CALL ats_vm_save( ats_vm* vm, ats_write_fn write, void* user )
{
	if( !vm || !write ) return ATS_ERR_INVALID_ARG;
	if( vm->in_update ) return ATS_ERR_BUSY;
	const ats_runtime* rt = vm->rt;

	Writer w;
	w.fn   = write;
	w.user = user;
	w.U32( kSaveMagic );
	w.U16( kSaveMajor );
	w.U16( kSaveMinor );
	w.U64( vm->rng );

	uint32_t n = 0;
	for( uint32_t i = 0; i < rt->vars.Size(); ++i ) if( rt->vars[i].scope == ATS_SCOPE_PERSISTENT ) ++n;
	w.U32( n );
	for( uint32_t i = 0; i < rt->vars.Size(); ++i ){
		const VarDef& d = rt->vars[i];
		if( d.scope != ATS_SCOPE_PERSISTENT ) continue;
		w.U32( d.id );
		w.Value( vm, d.type, vm->shared[i], nullptr );
	}

	w.U32( vm->blocks.Size() );
	for( uint32_t i = 0; i < vm->blocks.Size(); ++i ){
		const ScriptBlock* blk = vm->blocks[i];
		uint32_t count = blk->dormant.Size();
		for( uint32_t k = 0; k < blk->vars.Size(); ++k ) if( !(blk->vars[k].flags & kVarTransient) ) ++count;
		w.Str( blk->script_id );
		w.U32( count );
		for( uint32_t k = 0; k < blk->vars.Size(); ++k ){
			const ScriptVar& sv = blk->vars[k];
			if( sv.flags & kVarTransient ) continue;
			w.Str( sv.name );
			w.Value( vm, sv.type, sv.value, nullptr );
		}
		// まだ読み込まれていないスクリプトの値もそのまま書き戻す
		for( uint32_t k = 0; k < blk->dormant.Size(); ++k ){
			const DormantVar& d = blk->dormant[k];
			w.Str( d.name );
			w.Value( vm, d.type, d.value, d.str );
		}
	}
	return w.r;
}

//=========================================================================
// ロード
//=========================================================================
namespace {

struct SavedVar {
	char*		script;		// nullptr なら共有変数
	char*		name;
	uint32_t	id;
	uint32_t	type;
	uint64_t	value;
	char*		str;
};

struct Reader {
	ats_read_fn			fn;
	void*				user;
	const Allocator*	alloc;
	ats_result			r = ATS_OK;

	bool Bytes( void* p, size_t n ) { if( r == ATS_OK && n ) r = fn( p, n, user ); return r == ATS_OK; }
	uint8_t  U8()  { uint8_t v = 0;  Bytes( &v, 1 ); return v; }
	uint16_t U16() { uint16_t v = 0; Bytes( &v, 2 ); return v; }
	uint32_t U32() { uint32_t v = 0; Bytes( &v, 4 ); return v; }
	uint64_t U64() { uint64_t v = 0; Bytes( &v, 8 ); return v; }
	char* Str()
	{
		uint32_t n = U32();
		if( r != ATS_OK ) return nullptr;
		if( n > kMaxSaveString ){ r = ATS_ERR_BAD_FORMAT; return nullptr; }
		char* s = (char*)alloc->Alloc( n + 1, 1 );
		if( !s ){ r = ATS_ERR_OUT_OF_MEMORY; return nullptr; }
		if( !Bytes( s, n ) ){ alloc->Free( s ); return nullptr; }
		s[n] = '\0';
		return s;
	}
	bool Value( SavedVar& sv )
	{
		sv.type = U8();
		if( r != ATS_OK ) return false;
		if( !IsValidType( sv.type ) ){ r = ATS_ERR_BAD_FORMAT; return false; }
		if( sv.type == ATS_TYPE_STRING ){ sv.str = Str(); return sv.str != nullptr; }
		sv.value = U64();
		return r == ATS_OK;
	}
};

void FreeSaved( const Allocator* a, Array<SavedVar>& list )
{
	for( uint32_t i = 0; i < list.Size(); ++i ){
		a->Free( list[i].name );
		a->Free( list[i].str );
	}
	// script 名は共有しているので重複して解放しない
	char* last = nullptr;
	for( uint32_t i = 0; i < list.Size(); ++i ){
		if( list[i].script && list[i].script != last ){ last = list[i].script; a->Free( last ); }
	}
	list.Clear();
}

}	// namespace

extern "C" ATS_API ats_result ATS_CALL ats_vm_load( ats_vm* vm, ats_read_fn read, void* user )
{
	if( !vm || !read ) return ATS_ERR_INVALID_ARG;
	if( vm->in_update ) return ATS_ERR_BUSY;
	const Allocator* a = vm->alloc;
	const ats_runtime* rt = vm->rt;

	// まず全体を読み込んで検証する（途中で失敗しても状態を変えない）
	Reader rd;
	rd.fn    = read;
	rd.user  = user;
	rd.alloc = a;
	Array<SavedVar> list;
	list.Init( a );

	auto fail = [&]( ats_result r ) { FreeSaved( a, list ); return r; };

	if( rd.U32() != kSaveMagic ) return fail( rd.r != ATS_OK ? rd.r : ATS_ERR_BAD_FORMAT );
	uint16_t major = rd.U16();
	rd.U16();
	if( rd.r != ATS_OK ) return fail( rd.r );
	if( major != kSaveMajor ) return fail( ATS_ERR_VERSION );
	uint64_t rng = rd.U64();

	uint32_t sharedCount = rd.U32();
	for( uint32_t i = 0; i < sharedCount && rd.r == ATS_OK; ++i ){
		SavedVar sv;
		memset( &sv, 0, sizeof(sv) );
		sv.id = rd.U32();
		if( !rd.Value( sv ) ) break;
		if( !list.Push( sv ) ){ a->Free( sv.str ); return fail( ATS_ERR_OUT_OF_MEMORY ); }
	}
	uint32_t blockCount = rd.U32();
	for( uint32_t i = 0; i < blockCount && rd.r == ATS_OK; ++i ){
		char* script = rd.Str();
		if( !script ) break;
		uint32_t count = rd.U32();
		bool owned = false;	// script 名を list に渡したか
		for( uint32_t k = 0; k < count && rd.r == ATS_OK; ++k ){
			SavedVar sv;
			memset( &sv, 0, sizeof(sv) );
			sv.script = script;
			sv.name   = rd.Str();
			if( !sv.name ) break;
			if( !rd.Value( sv ) ){ a->Free( sv.name ); break; }
			if( !list.Push( sv ) ){ a->Free( sv.name ); a->Free( sv.str ); rd.r = ATS_ERR_OUT_OF_MEMORY; break; }
			owned = true;
		}
		if( !owned ) a->Free( script );
	}
	if( rd.r != ATS_OK ) return fail( rd.r );

	// 実行中のファイバをすべて中断する（ロード後はファイバのない状態から始まる）
	for( uint32_t i = 0; i < vm->fibers.Size(); ++i ){
		Fiber* f = vm->fibers[i];
		if( f->state != FiberState::Free ) vm->AbortFiber( f, true );
	}

	// 初期値に戻してから適用する
	for( uint32_t i = 0; i < rt->vars.Size(); ++i ){
		const VarDef& d = rt->vars[i];
		if( d.scope != ATS_SCOPE_PERSISTENT ) continue;
		vm->shared[i] = (d.type == ATS_TYPE_STRING) ? vm->Intern( d.init_str ? d.init_str : "" ) : d.init;
	}
	for( uint32_t i = 0; i < vm->blocks.Size(); ++i ) vm->ResetBlock( vm->blocks[i] );
	vm->rng = rng ? rng : vm->rng;

	for( uint32_t i = 0; i < list.Size(); ++i ){
		SavedVar& sv = list[i];
		if( !sv.script ){
			int idx = rt->FindVar( sv.id );
			if( idx < 0 ){ vm->Log( ATS_LOG_WARNING, "saved variable id %u is no longer defined", sv.id ); continue; }
			const VarDef& d = rt->vars[ (uint32_t)idx ];
			if( d.scope != ATS_SCOPE_PERSISTENT ) continue;
			uint64_t v = sv.value;
			if( vm->ConvertSlot( &v, sv.type, d.type, sv.str ) ) vm->shared[ (uint32_t)idx ] = v;
			else vm->Log( ATS_LOG_WARNING, "saved variable '%s' has an incompatible type; using the initial value", d.name );
			continue;
		}

		ScriptBlock* blk = vm->GetBlock( sv.script );
		if( !blk ) return fail( ATS_ERR_OUT_OF_MEMORY );
		ScriptVar* target = nullptr;
		for( uint32_t k = 0; k < blk->vars.Size() && !target; ++k )
			if( strcmp( blk->vars[k].name, sv.name ) == 0 ) target = &blk->vars[k];
		for( uint32_t k = 0; k < blk->vars.Size() && !target; ++k )
			if( blk->vars[k].was_name && strcmp( blk->vars[k].was_name, sv.name ) == 0 ) target = &blk->vars[k];

		if( target ){
			uint64_t v = sv.value;
			if( vm->ConvertSlot( &v, sv.type, target->type, sv.str ) ) target->value = v;
			else vm->Log( ATS_LOG_WARNING, "[%s] saved variable '%s' has an incompatible type; using the initial value", blk->script_id, sv.name );
		} else {
			// まだ読み込まれていないスクリプト（または削除された変数）。休眠データとして持っておく
			DormantVar d;
			d.name  = sv.name;
			d.type  = sv.type;
			d.value = sv.value;
			d.str   = sv.str;
			if( !blk->dormant.Push( d ) ) return fail( ATS_ERR_OUT_OF_MEMORY );
			sv.name = nullptr;
			sv.str  = nullptr;
		}
	}

	FreeSaved( a, list );
	return ATS_OK;
}
