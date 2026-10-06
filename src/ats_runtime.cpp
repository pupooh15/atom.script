/**************************************************************************/
/*!	\file	ats_runtime.cpp
	\brief	ランタイム（コマンド・クエリ・共有変数の登録）
***************************************************************************/
#include "ats_internal.h"

#include <stdio.h>
#include <stdlib.h>

using namespace ats;

//=========================================================================
// 共通
//=========================================================================
namespace ats {

void FormatV( char* buf, size_t size, const char* fmt, va_list ap )
{
	vsnprintf( buf, size, fmt, ap );
	buf[ size - 1 ] = '\0';
}

void Format( char* buf, size_t size, const char* fmt, ... )
{
	va_list ap;
	va_start( ap, fmt );
	FormatV( buf, size, fmt, ap );
	va_end( ap );
}

}	// namespace ats

static void* ATS_CALL DefaultAlloc( size_t size, size_t align, void* )
{
	// 標準の malloc は 16 バイト境界までを保証する前提
	if( align > 16 ) return nullptr;
	return malloc( size ? size : 1 );
}

static void ATS_CALL DefaultFree( void* p, void* )
{
	free( p );
}

void ats_runtime::Log( ats_log_level level, const char* fmt, ... )
{
	if( !log ) return;
	char buf[ kErrorSize ];
	va_list ap;
	va_start( ap, fmt );
	FormatV( buf, sizeof(buf), fmt, ap );
	va_end( ap );
	log( level, buf, log_user );
}

int ats_runtime::FindVar( uint32_t id ) const
{
	for( uint32_t i = 0; i < vars.Size(); ++i ) if( vars[i].id == id ) return (int)i;
	return -1;
}

static ats_result RtError( ats_runtime* rt, ats_result r, const char* fmt, ... )
{
	va_list ap;
	va_start( ap, fmt );
	FormatV( rt->error, sizeof(rt->error), fmt, ap );
	va_end( ap );
	return r;
}

//=========================================================================
// バージョン・エラー
//=========================================================================
extern "C" ATS_API uint32_t ATS_CALL ats_get_api_version( void )
{
	return ((uint32_t)ATS_API_VERSION_MAJOR << 16) | ATS_API_VERSION_MINOR;
}

extern "C" ATS_API const char* ATS_CALL ats_result_string( ats_result r )
{
	switch( r ){
	case ATS_OK:				return "ok";
	case ATS_ERR_INVALID_ARG:	return "invalid argument";
	case ATS_ERR_OUT_OF_MEMORY:	return "out of memory";
	case ATS_ERR_BAD_FORMAT:	return "bad format";
	case ATS_ERR_VERSION:		return "version mismatch";
	case ATS_ERR_UNRESOLVED:	return "unresolved import";
	case ATS_ERR_SIGNATURE:		return "signature mismatch";
	case ATS_ERR_NOT_FOUND:		return "not found";
	case ATS_ERR_STALE_ID:		return "stale id";
	case ATS_ERR_TYPE:			return "type mismatch";
	case ATS_ERR_BUSY:			return "busy";
	case ATS_ERR_DUPLICATE:		return "duplicate";
	}
	return "unknown";
}

//=========================================================================
// ランタイム
//=========================================================================
extern "C" ATS_API ats_result ATS_CALL ats_runtime_create( const ats_runtime_desc* desc, ats_runtime** out )
{
	if( !out ) return ATS_ERR_INVALID_ARG;
	*out = nullptr;
	if( desc && desc->size < sizeof(ats_runtime_desc) ) return ATS_ERR_INVALID_ARG;
	if( desc && (!desc->alloc) != (!desc->free) ) return ATS_ERR_INVALID_ARG;

	Allocator a;
	a.alloc = (desc && desc->alloc) ? desc->alloc : DefaultAlloc;
	a.free  = (desc && desc->free)  ? desc->free  : DefaultFree;
	a.user  = desc ? desc->user : nullptr;

	ats_runtime* rt = a.New<ats_runtime>();
	if( !rt ) return ATS_ERR_OUT_OF_MEMORY;
	rt->alloc    = a;
	rt->log      = desc ? desc->log : nullptr;
	rt->log_user = desc ? desc->user : nullptr;
	rt->error[0] = '\0';
	rt->commands.Init( &rt->alloc );
	rt->queries.Init( &rt->alloc );
	rt->vars.Init( &rt->alloc );
	rt->channels.Init( &rt->alloc );
	rt->command_map.Init( &rt->alloc );
	rt->query_map.Init( &rt->alloc );
	rt->channel_map.Init( &rt->alloc );
	*out = rt;
	return ATS_OK;
}

extern "C" ATS_API void ATS_CALL ats_runtime_destroy( ats_runtime* rt )
{
	if( !rt ) return;
	const Allocator a = rt->alloc;
	for( uint32_t i = 0; i < rt->commands.Size(); ++i ) a.Free( rt->commands[i].name );
	for( uint32_t i = 0; i < rt->queries.Size(); ++i ) a.Free( rt->queries[i].name );
	for( uint32_t i = 0; i < rt->vars.Size(); ++i ){ a.Free( rt->vars[i].name ); a.Free( rt->vars[i].init_str ); }
	for( uint32_t i = 0; i < rt->channels.Size(); ++i ) a.Free( rt->channels[i] );
	// メンバの Array/StrMap は rt->alloc を参照しているので、先に破棄してから本体を解放する
	rt->~ats_runtime();
	a.Free( rt );
}

extern "C" ATS_API const char* ATS_CALL ats_runtime_last_error( ats_runtime* rt )
{
	return rt ? rt->error : "";
}

extern "C" ATS_API ats_result ATS_CALL ats_register_command( ats_runtime* rt, const ats_command_desc* desc )
{
	if( !rt || !desc || desc->size < sizeof(ats_command_desc) || !desc->name || !desc->fn ) return ATS_ERR_INVALID_ARG;
	uint32_t dummy;
	if( rt->command_map.Find( desc->name, &dummy ) )
		return RtError( rt, ATS_ERR_DUPLICATE, "command '%s' is already registered", desc->name );

	CommandDef d;
	d.name     = rt->alloc.StrDup( desc->name );
	d.sig_hash = desc->sig_hash;
	d.channel  = fmt::kNone;
	d.fn       = desc->fn;
	d.cancel   = desc->cancel;
	d.user     = desc->user;
	if( !d.name ) return ATS_ERR_OUT_OF_MEMORY;

	if( desc->channel && desc->channel[0] ){
		uint32_t ch;
		if( !rt->channel_map.Find( desc->channel, &ch ) ){
			char* name = rt->alloc.StrDup( desc->channel );
			if( !name || !rt->channels.Push( name ) ){ rt->alloc.Free( name ); rt->alloc.Free( d.name ); return ATS_ERR_OUT_OF_MEMORY; }
			ch = rt->channels.Size() - 1;
			if( !rt->channel_map.Insert( name, ch ) ){ rt->alloc.Free( d.name ); return ATS_ERR_OUT_OF_MEMORY; }
		}
		d.channel = ch;
	}

	if( !rt->commands.Push( d ) ){ rt->alloc.Free( d.name ); return ATS_ERR_OUT_OF_MEMORY; }
	if( !rt->command_map.Insert( d.name, rt->commands.Size() - 1 ) ) return ATS_ERR_OUT_OF_MEMORY;
	return ATS_OK;
}

extern "C" ATS_API ats_result ATS_CALL ats_register_query( ats_runtime* rt, const ats_query_desc* desc )
{
	if( !rt || !desc || desc->size < sizeof(ats_query_desc) || !desc->name || !desc->fn ) return ATS_ERR_INVALID_ARG;
	uint32_t dummy;
	if( rt->query_map.Find( desc->name, &dummy ) )
		return RtError( rt, ATS_ERR_DUPLICATE, "query '%s' is already registered", desc->name );

	QueryDef d;
	d.name     = rt->alloc.StrDup( desc->name );
	d.sig_hash = desc->sig_hash;
	d.fn       = desc->fn;
	d.user     = desc->user;
	if( !d.name ) return ATS_ERR_OUT_OF_MEMORY;
	if( !rt->queries.Push( d ) ){ rt->alloc.Free( d.name ); return ATS_ERR_OUT_OF_MEMORY; }
	if( !rt->query_map.Insert( d.name, rt->queries.Size() - 1 ) ) return ATS_ERR_OUT_OF_MEMORY;
	return ATS_OK;
}

extern "C" ATS_API ats_result ATS_CALL ats_define_var( ats_runtime* rt, const ats_var_desc* desc )
{
	if( !rt || !desc || desc->size < sizeof(ats_var_desc) || !desc->name ) return ATS_ERR_INVALID_ARG;
	if( !IsValidType( desc->type ) ) return ATS_ERR_INVALID_ARG;
	if( desc->scope != ATS_SCOPE_PERSISTENT && desc->scope != ATS_SCOPE_SESSION ) return ATS_ERR_INVALID_ARG;
	if( rt->FindVar( desc->id ) >= 0 )
		return RtError( rt, ATS_ERR_DUPLICATE, "variable id %u is already defined", desc->id );
	if( desc->init.type != ATS_TYPE_VOID && desc->init.type != desc->type &&
		!(IsIntLike( desc->init.type ) && IsIntLike( desc->type )) )
		return RtError( rt, ATS_ERR_TYPE, "variable '%s': initial value type mismatch", desc->name );

	VarDef d;
	memset( &d, 0, sizeof(d) );
	d.id    = desc->id;
	d.type  = desc->type;
	d.scope = desc->scope;
	d.name  = rt->alloc.StrDup( desc->name );
	if( !d.name ) return ATS_ERR_OUT_OF_MEMORY;
	if( desc->init.type != ATS_TYPE_VOID ){
		switch( desc->type ){
		case ATS_TYPE_BOOL:		d.init = desc->init.v.b ? 1 : 0; break;
		case ATS_TYPE_INT:
		case ATS_TYPE_ENUM:		d.init = FromI( desc->init.v.i ); break;
		case ATS_TYPE_FLOAT:	d.init = FromF( desc->init.v.f ); break;
		case ATS_TYPE_HANDLE:	d.init = desc->init.v.h; break;
		case ATS_TYPE_STRING:
			d.init_str = rt->alloc.StrDup( desc->init.v.s ? desc->init.v.s : "" );
			if( !d.init_str ){ rt->alloc.Free( d.name ); return ATS_ERR_OUT_OF_MEMORY; }
			break;
		}
	}
	if( !rt->vars.Push( d ) ){ rt->alloc.Free( d.name ); rt->alloc.Free( d.init_str ); return ATS_ERR_OUT_OF_MEMORY; }
	return ATS_OK;
}
