/**************************************************************************/
/*!	\file	ats_program.cpp
	\brief	プログラム（.atsb）の読み込みと検証
	\note
	壊れたデータでメモリを壊さないよう、読み込み時にすべての参照と
	スタック深さを検証する。検証に通ったコードは実行時の範囲検査を減らせる。
***************************************************************************/
#include "ats_internal.h"

#include <stdio.h>

using namespace ats;
using namespace ats::fmt;

//=========================================================================
// 読み込み補助
//=========================================================================
namespace {

template<class T> T Read( const uint8_t* p ) { T v; memcpy( &v, p, sizeof(T) ); return v; }

struct Loader {
	ats_runtime*	rt;
	ats_program*	prog;
	char			unresolved[ kErrorSize ];
	size_t			unresolved_len = 0;
	bool			has_unresolved = false;

	ats_result Fail( ats_result r, const char* fmt, ... )
	{
		va_list ap;
		va_start( ap, fmt );
		FormatV( rt->error, sizeof(rt->error), fmt, ap );
		va_end( ap );
		return r;
	}

	void AddUnresolved( const char* kind, const char* name )
	{
		has_unresolved = true;
		if( unresolved_len + 64 >= sizeof(unresolved) ) return;
		int n = snprintf( unresolved + unresolved_len, sizeof(unresolved) - unresolved_len,
						  "%s%s '%s'", unresolved_len ? ", " : "", kind, name );
		if( n > 0 ) unresolved_len += (size_t)n;
	}

	bool StrOk( uint32_t idx ) const { return idx < prog->strings.Size(); }
};

}	// namespace

//=========================================================================
// セクションの解析
//=========================================================================
static ats_result ParseStrings( Loader& L, const uint8_t* p, uint32_t size )
{
	if( size < 4 ) return L.Fail( ATS_ERR_BAD_FORMAT, "STRS: too small" );
	uint32_t count = Read<uint32_t>( p );
	if( count > (size - 4) / 4 ) return L.Fail( ATS_ERR_BAD_FORMAT, "STRS: bad count" );
	if( !L.prog->strings.Resize( count ) ) return ATS_ERR_OUT_OF_MEMORY;
	for( uint32_t i = 0; i < count; ++i ){
		uint32_t off = Read<uint32_t>( p + 4 + i * 4 );
		if( off < 4 + count * 4 || off >= size ) return L.Fail( ATS_ERR_BAD_FORMAT, "STRS: bad offset %u", i );
		if( !memchr( p + off, 0, size - off ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "STRS: string %u not terminated", i );
		L.prog->strings[i] = (const char*)(p + off);
	}
	return ATS_OK;
}

template<class T>
static ats_result ParseTable( Loader& L, const uint8_t* p, uint32_t size, Array<T>& out, const char* tag )
{
	if( size < 4 ) return L.Fail( ATS_ERR_BAD_FORMAT, "%s: too small", tag );
	uint32_t count = Read<uint32_t>( p );
	if( count > (size - 4) / sizeof(T) ) return L.Fail( ATS_ERR_BAD_FORMAT, "%s: bad count", tag );
	if( !out.Resize( count ) ) return ATS_ERR_OUT_OF_MEMORY;
	if( count ) memcpy( (void*)out.Data(), p + 4, sizeof(T) * count );
	return ATS_OK;
}

static ats_result ResolveImports( Loader& L )
{
	ats_program* prog = L.prog;
	ats_runtime* rt   = L.rt;
	if( !prog->resolved.Resize( prog->imports.Size() ) ) return ATS_ERR_OUT_OF_MEMORY;

	for( uint32_t i = 0; i < prog->imports.Size(); ++i ){
		const ImportEntry& e = prog->imports[i];
		if( !L.StrOk( e.name ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "IMPT %u: bad name", i );
		if( e.argc > kMaxParams || e.retc > 1 ) return L.Fail( ATS_ERR_BAD_FORMAT, "IMPT %u: bad arity", i );
		for( int a = 0; a < e.argc; ++a )
			if( !IsValidType( e.param_types[a] ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "IMPT %u: bad param type", i );
		if( e.retc && !IsValidType( e.ret_type ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "IMPT %u: bad return type", i );

		const char* name = prog->strings[ e.name ];
		uint32_t idx;
		if( e.kind == kImportCommand ){
			if( !rt->command_map.Find( name, &idx ) ){ L.AddUnresolved( "command", name ); continue; }
			const CommandDef& c = rt->commands[ idx ];
			if( c.sig_hash && e.sig_hash && c.sig_hash != e.sig_hash )
				return L.Fail( ATS_ERR_SIGNATURE, "command '%s': signature mismatch", name );
		} else if( e.kind == kImportQuery ){
			if( !e.retc ) return L.Fail( ATS_ERR_BAD_FORMAT, "IMPT %u: query must return a value", i );
			if( !rt->query_map.Find( name, &idx ) ){ L.AddUnresolved( "query", name ); continue; }
			const QueryDef& q = rt->queries[ idx ];
			if( q.sig_hash && e.sig_hash && q.sig_hash != e.sig_hash )
				return L.Fail( ATS_ERR_SIGNATURE, "query '%s': signature mismatch", name );
		} else {
			return L.Fail( ATS_ERR_BAD_FORMAT, "IMPT %u: bad kind", i );
		}
		prog->resolved[i].kind  = e.kind;
		prog->resolved[i].index = idx;
	}
	return ATS_OK;
}

static ats_result ResolveVars( Loader& L )
{
	ats_program* prog = L.prog;
	if( !prog->var_shared.Resize( prog->vars.Size() ) ) return ATS_ERR_OUT_OF_MEMORY;

	for( uint32_t i = 0; i < prog->vars.Size(); ++i ){
		const VarEntry& v = prog->vars[i];
		prog->var_shared[i] = -1;
		if( !IsValidType( v.type ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "VARS %u: bad type", i );
		if( v.kind == kVarShared ){
			int idx = L.rt->FindVar( v.id );
			if( idx < 0 ){
				char name[32];
				Format( name, sizeof(name), "id %u", v.id );
				L.AddUnresolved( "variable", name );
				continue;
			}
			const VarDef& d = L.rt->vars[ (uint32_t)idx ];
			if( d.type != v.type && !(IsIntLike( d.type ) && IsIntLike( v.type )) )
				return L.Fail( ATS_ERR_TYPE, "variable '%s': type mismatch", d.name );
			prog->var_shared[i] = idx;
		} else if( v.kind == kVarScript ){
			if( !L.StrOk( v.name ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "VARS %u: bad name", i );
			if( v.was_name != kNone && !L.StrOk( v.was_name ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "VARS %u: bad was_name", i );
			if( v.type == ATS_TYPE_STRING && !L.StrOk( (uint32_t)v.init ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "VARS %u: bad init string", i );
			for( uint32_t j = 0; j < i; ++j ){
				const VarEntry& o = prog->vars[j];
				if( o.kind == kVarScript && strcmp( prog->strings[ o.name ], prog->strings[ v.name ] ) == 0 )
					return L.Fail( ATS_ERR_BAD_FORMAT, "VARS %u: duplicate name '%s'", i, prog->strings[ v.name ] );
			}
		} else {
			return L.Fail( ATS_ERR_BAD_FORMAT, "VARS %u: bad kind", i );
		}
	}
	return ATS_OK;
}

static ats_result CheckEntries( Loader& L )
{
	ats_program* prog = L.prog;
	for( uint32_t i = 0; i < prog->entries.Size(); ++i ){
		const EntryEntry& e = prog->entries[i];
		if( !L.StrOk( e.name ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "ENTR %u: bad name", i );
		if( e.kind != kEntryEvent && e.kind != kEntryFunction ) return L.Fail( ATS_ERR_BAD_FORMAT, "ENTR %u: bad kind", i );
		if( e.code >= prog->code_size ) return L.Fail( ATS_ERR_BAD_FORMAT, "ENTR %u: bad code offset", i );
		if( e.paramc > kMaxParams || e.paramc > e.localc || e.retc > 1 ) return L.Fail( ATS_ERR_BAD_FORMAT, "ENTR %u: bad arity", i );
		if( e.kind == kEntryEvent && e.retc ) return L.Fail( ATS_ERR_BAD_FORMAT, "ENTR %u: event cannot return a value", i );
		for( int a = 0; a < e.paramc; ++a )
			if( !IsValidType( e.param_types[a] ) ) return L.Fail( ATS_ERR_BAD_FORMAT, "ENTR %u: bad param type", i );
	}
	return ATS_OK;
}

//=========================================================================
// コード検証（制御フローに沿ってスタック深さを追う）
//=========================================================================
namespace {

struct Verifier {
	Loader&				L;
	const ats_program*	prog;
	Array<int32_t>		depth;		// 各バイト位置での深さ（命令の先頭のみ有効。-1 未到達）
	Array<uint8_t>		interior;	// 命令の途中のバイトなら 1
	Array<uint32_t>		work;		// 未処理の命令位置

	explicit Verifier( Loader& l ) : L( l ), prog( l.prog ) {}

	ats_result Init()
	{
		depth.Init( &L.rt->alloc );
		interior.Init( &L.rt->alloc );
		work.Init( &L.rt->alloc );
		if( !depth.Resize( prog->code_size ) || !interior.Resize( prog->code_size ) ) return ATS_ERR_OUT_OF_MEMORY;
		return ATS_OK;
	}

	ats_result Run( uint32_t entryIndex );
};

}	// namespace

ats_result Verifier::Run( uint32_t entryIndex )
{
	const EntryEntry& e = prog->entries[ entryIndex ];
	const uint8_t* code = prog->code;
	const uint32_t size = prog->code_size;

	// depth は「深さ + 1」を記録する（0 は未到達）。エントリごとに作り直す
	memset( depth.Data(), 0, sizeof(int32_t) * size );
	work.Clear();

	auto push = [&]( uint32_t pc, int32_t d, uint32_t from ) -> ats_result {
		if( pc >= size ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: jump out of range", prog->strings[e.name], from );
		if( d < 0 ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: stack underflow", prog->strings[e.name], from );
		if( d > (int32_t)e.max_stack ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: stack exceeds max_stack", prog->strings[e.name], from );
		if( depth[pc] == 0 ){
			depth[pc] = d + 1;
			if( !work.Push( pc ) ) return ATS_ERR_OUT_OF_MEMORY;
		} else if( depth[pc] != d + 1 ){
			return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: inconsistent stack depth", prog->strings[e.name], pc );
		}
		return ATS_OK;
	};

	ats_result r = push( e.code, 0, e.code );
	if( r != ATS_OK ) return r;

	while( !work.Empty() ){
		uint32_t pc = work.Back();
		work.Pop();
		int32_t d = depth[pc] - 1;
		uint8_t op = code[pc];
		const char* en = prog->strings[ e.name ];

		if( op >= OP_COUNT ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: bad opcode %u", en, pc, op );
		uint32_t len = 1 + (uint32_t)OperandSize( op );
		if( pc + len > size ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: truncated", en, pc );
		const uint8_t* a = code + pc + 1;
		if( op == OP_SWITCH ){
			uint16_t count = Read<uint16_t>( a );
			len += (uint32_t)count * 8;
			if( pc + len > size ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: truncated switch", en, pc );
		}
		for( uint32_t k = 1; k < len; ++k ) interior[ pc + k ] = 1;
		const uint32_t next = pc + len;

		int32_t pops = 0, pushes = 0;
		bool fall = true;

		switch( op ){
		case OP_NOP: break;
		case OP_PUSH_I32: case OP_PUSH_F32: pushes = 1; break;
		case OP_PUSH_K:
			if( Read<uint32_t>( a ) >= prog->consts.Size() ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: bad constant", en, pc );
			pushes = 1; break;
		case OP_PUSH_STR:
			if( Read<uint32_t>( a ) >= prog->strings.Size() ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: bad string", en, pc );
			pushes = 1; break;
		case OP_POP: pops = 1; break;
		case OP_DUP: pops = 1; pushes = 2; break;
		case OP_LD_LOCAL: case OP_ST_LOCAL:
			if( Read<uint16_t>( a ) >= e.localc ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: bad local", en, pc );
			if( op == OP_LD_LOCAL ) pushes = 1; else pops = 1;
			break;
		case OP_LD_VAR: case OP_ST_VAR:
			if( Read<uint16_t>( a ) >= prog->vars.Size() ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: bad variable", en, pc );
			if( op == OP_LD_VAR ) pushes = 1; else pops = 1;
			break;
		case OP_ADD_I: case OP_SUB_I: case OP_MUL_I: case OP_DIV_I: case OP_MOD_I:
		case OP_ADD_F: case OP_SUB_F: case OP_MUL_F: case OP_DIV_F:
		case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
		case OP_EQ_I: case OP_NE_I: case OP_LT_I: case OP_LE_I: case OP_GT_I: case OP_GE_I:
		case OP_EQ_F: case OP_NE_F: case OP_LT_F: case OP_LE_F: case OP_GT_F: case OP_GE_F:
		case OP_EQ_H: case OP_NE_H:
			pops = 2; pushes = 1; break;
		case OP_NEG_I: case OP_NEG_F: case OP_BNOT: case OP_NOT: case OP_I2F: case OP_F2I: case OP_RAND:
			pops = 1; pushes = 1; break;
		case OP_JMP:
			fall = false;
			r = push( next + (uint32_t)Read<int32_t>( a ), d, pc );
			if( r != ATS_OK ) return r;
			break;
		case OP_JZ: case OP_JNZ:
			pops = 1;
			r = push( next + (uint32_t)Read<int32_t>( a ), d - 1, pc );
			if( r != ATS_OK ) return r;
			break;
		case OP_SWITCH: {
			pops = 1;
			fall = false;
			uint16_t count = Read<uint16_t>( a );
			r = push( next + (uint32_t)Read<int32_t>( a + 2 ), d - 1, pc );
			if( r != ATS_OK ) return r;
			for( uint32_t k = 0; k < count; ++k ){
				r = push( next + (uint32_t)Read<int32_t>( a + 6 + k * 8 + 4 ), d - 1, pc );
				if( r != ATS_OK ) return r;
			}
			break;
		}
		case OP_CALL_CMD: case OP_CALL_QUERY: {
			uint16_t imp = Read<uint16_t>( a );
			uint8_t argc = a[2];
			if( imp >= prog->imports.Size() ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: bad import", en, pc );
			const ImportEntry& ie = prog->imports[ imp ];
			uint8_t want = (op == OP_CALL_CMD) ? kImportCommand : kImportQuery;
			if( ie.kind != want || ie.argc != argc ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: import mismatch", en, pc );
			pops = argc; pushes = ie.retc;
			break;
		}
		case OP_CALL_FN: {
			uint16_t fn = Read<uint16_t>( a );
			if( fn >= prog->entries.Size() || prog->entries[fn].kind != kEntryFunction )
				return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: bad function", en, pc );
			pops = prog->entries[fn].paramc; pushes = prog->entries[fn].retc;
			break;
		}
		case OP_RET:
			fall = false;
			if( d < (int32_t)e.retc ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: missing return value", en, pc );
			break;
		case OP_END:
			fall = false;
			break;
		case OP_FORK:
			// 子ファイバは空の演算スタックで開始する
			r = push( next + (uint32_t)Read<int32_t>( a ), 0, pc );
			if( r != ATS_OK ) return r;
			break;
		case OP_JOIN: case OP_RACE: case OP_YIELD: case OP_RESET_VARS: break;
		case OP_SLEEP: case OP_WAIT_FRAMES: pops = 1; break;
		case OP_FIRE:
			if( Read<uint32_t>( a ) >= prog->strings.Size() || a[4] > kMaxParams )
				return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: bad fire", en, pc );
			pops = a[4];
			break;
		default:
			return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: bad opcode %u", en, pc, op );
		}

		if( d < pops ) return L.Fail( ATS_ERR_BAD_FORMAT, "entry '%s' code %u: stack underflow", en, pc );
		if( fall ){
			r = push( next, d - pops + pushes, pc );
			if( r != ATS_OK ) return r;
		}
	}

	// 命令の途中に飛び込む経路がないか
	for( uint32_t pc = 0; pc < size; ++pc )
		if( depth[pc] && interior[pc] ) return L.Fail( ATS_ERR_BAD_FORMAT, "code %u: jump into the middle of an instruction", pc );
	return ATS_OK;
}

//=========================================================================
// 公開関数
//=========================================================================
static void DestroyProgram( ats_program* prog )
{
	const Allocator a = prog->rt->alloc;
	void* bytes = prog->bytes;
	prog->~ats_program();
	a.Free( bytes );
	a.Free( prog );
}

extern "C" ATS_API ats_result ATS_CALL ats_program_load( ats_runtime* rt, const void* bytes, size_t size, ats_program** out )
{
	if( !rt || !bytes || !out ) return ATS_ERR_INVALID_ARG;
	*out = nullptr;
	if( size < sizeof(FileHeader) || size > 0x7FFFFFFF ) { Format( rt->error, sizeof(rt->error), "file too small" ); return ATS_ERR_BAD_FORMAT; }

	ats_program* prog = rt->alloc.New<ats_program>();
	if( !prog ) return ATS_ERR_OUT_OF_MEMORY;
	prog->rt        = rt;
	prog->refcount  = 1;
	prog->size      = (uint32_t)size;
	prog->code      = nullptr;
	prog->code_size = 0;
	prog->script_id = "";
	prog->bytes     = (uint8_t*)rt->alloc.Alloc( size, 8 );
	prog->strings.Init( &rt->alloc );
	prog->consts.Init( &rt->alloc );
	prog->imports.Init( &rt->alloc );
	prog->resolved.Init( &rt->alloc );
	prog->vars.Init( &rt->alloc );
	prog->var_shared.Init( &rt->alloc );
	prog->entries.Init( &rt->alloc );
	prog->debug.Init( &rt->alloc );
	if( !prog->bytes ){ DestroyProgram( prog ); return ATS_ERR_OUT_OF_MEMORY; }
	memcpy( prog->bytes, bytes, size );

	Loader L;
	L.rt   = rt;
	L.prog = prog;
	L.unresolved[0] = '\0';

	auto fail = [&]( ats_result r ) { DestroyProgram( prog ); return r; };

	const uint8_t* p = prog->bytes;
	FileHeader h = Read<FileHeader>( p );
	if( h.magic != kMagic ) return fail( L.Fail( ATS_ERR_BAD_FORMAT, "not an .atsb file" ) );
	if( h.version_major != kVersionMajor )
		return fail( L.Fail( ATS_ERR_VERSION, "format version %u.%u is not supported", h.version_major, h.version_minor ) );
	if( h.file_size > size ) return fail( L.Fail( ATS_ERR_BAD_FORMAT, "file is truncated" ) );
	if( h.section_count > 256 || sizeof(FileHeader) + (size_t)h.section_count * sizeof(SectionEntry) > h.file_size )
		return fail( L.Fail( ATS_ERR_BAD_FORMAT, "bad section table" ) );

	const uint8_t* sec[7]  = {};
	uint32_t       secs[7] = {};
	const uint32_t tags[7] = { kSecStrings, kSecConsts, kSecImports, kSecVars, kSecEntries, kSecCode, kSecDebug };
	for( uint32_t i = 0; i < h.section_count; ++i ){
		SectionEntry s = Read<SectionEntry>( p + sizeof(FileHeader) + i * sizeof(SectionEntry) );
		if( (uint64_t)s.offset + s.size > h.file_size ) return fail( L.Fail( ATS_ERR_BAD_FORMAT, "section %u out of range", i ) );
		for( int t = 0; t < 7; ++t ){
			if( s.tag != tags[t] ) continue;
			if( sec[t] ) return fail( L.Fail( ATS_ERR_BAD_FORMAT, "duplicate section" ) );
			sec[t]  = p + s.offset;
			secs[t] = s.size;
		}
		// 未知のセクションは読み飛ばす（minor バージョンでの追加に備える）
	}
	if( !sec[0] || !sec[4] || !sec[5] ) return fail( L.Fail( ATS_ERR_BAD_FORMAT, "required section is missing" ) );

	ats_result r;
	if( (r = ParseStrings( L, sec[0], secs[0] )) != ATS_OK ) return fail( r );
	if( !L.StrOk( h.script_id ) ) return fail( L.Fail( ATS_ERR_BAD_FORMAT, "bad script id" ) );
	prog->script_id = prog->strings[ h.script_id ];
	if( !prog->script_id[0] ) return fail( L.Fail( ATS_ERR_BAD_FORMAT, "script id is empty" ) );

	if( sec[1] && (r = ParseTable( L, sec[1], secs[1], prog->consts, "CNST" )) != ATS_OK ) return fail( r );
	if( sec[2] && (r = ParseTable( L, sec[2], secs[2], prog->imports, "IMPT" )) != ATS_OK ) return fail( r );
	if( sec[3] && (r = ParseTable( L, sec[3], secs[3], prog->vars, "VARS" )) != ATS_OK ) return fail( r );
	if( (r = ParseTable( L, sec[4], secs[4], prog->entries, "ENTR" )) != ATS_OK ) return fail( r );
	if( sec[6] && (r = ParseTable( L, sec[6], secs[6], prog->debug, "DBUG" )) != ATS_OK ) return fail( r );
	prog->code      = sec[5];
	prog->code_size = secs[5];
	if( prog->imports.Size() > 0xFFFF || prog->vars.Size() > 0xFFFF || prog->entries.Size() > 0xFFFF )
		return fail( L.Fail( ATS_ERR_BAD_FORMAT, "too many table entries" ) );

	if( (r = ResolveImports( L )) != ATS_OK ) return fail( r );
	if( (r = ResolveVars( L )) != ATS_OK ) return fail( r );
	if( L.has_unresolved ) return fail( L.Fail( ATS_ERR_UNRESOLVED, "unresolved: %s", L.unresolved ) );
	if( (r = CheckEntries( L )) != ATS_OK ) return fail( r );

	Verifier v( L );
	if( (r = v.Init()) != ATS_OK ) return fail( r );
	for( uint32_t i = 0; i < prog->entries.Size(); ++i )
		if( (r = v.Run( i )) != ATS_OK ) return fail( r );

	*out = prog;
	return ATS_OK;
}

extern "C" ATS_API void ATS_CALL ats_program_release( ats_program* prog )
{
	if( !prog ) return;
	if( --prog->refcount == 0 ) DestroyProgram( prog );
}

extern "C" ATS_API const char* ATS_CALL ats_program_script_id( const ats_program* prog )
{
	return prog ? prog->script_id : "";
}

int ats_program::FindEntry( const char* name, uint8_t kind ) const
{
	for( uint32_t i = 0; i < entries.Size(); ++i )
		if( entries[i].kind == kind && strcmp( strings[ entries[i].name ], name ) == 0 ) return (int)i;
	return -1;
}

uint32_t ats_program::LineOf( uint32_t pc ) const
{
	uint32_t line = 0;
	for( uint32_t i = 0; i < debug.Size(); ++i ){
		if( debug[i].pc > pc ) break;
		line = debug[i].line;
	}
	return line;
}
