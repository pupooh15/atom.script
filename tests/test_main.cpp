/**************************************************************************/
/*!	\file	test_main.cpp
	\brief	テストの実行と共通の環境
***************************************************************************/
#include "test_framework.h"

#include <cstdlib>

int g_failures = 0;

std::vector<TestCase>& AllTests()
{
	static std::vector<TestCase> tests;
	return tests;
}

//=========================================================================
// 値
//=========================================================================
ats_value V_Int( int32_t v )		{ ats_value x{}; x.type = ATS_TYPE_INT; x.v.i = v; return x; }
ats_value V_Float( float v )		{ ats_value x{}; x.type = ATS_TYPE_FLOAT; x.v.f = v; return x; }
ats_value V_Bool( bool v )			{ ats_value x{}; x.type = ATS_TYPE_BOOL; x.v.b = v ? 1 : 0; return x; }
ats_value V_Str( const char* s )	{ ats_value x{}; x.type = ATS_TYPE_STRING; x.v.s = s; return x; }
ats_value V_Handle( uint64_t h )	{ ats_value x{}; x.type = ATS_TYPE_HANDLE; x.v.h = h; return x; }

//=========================================================================
// 環境
//=========================================================================
static void ATS_CALL LogFn( ats_log_level level, const char* msg, void* user )
{
	Env* env = static_cast<Env*>( user );
	if( level == ATS_LOG_ERROR ) env->errors.push_back( msg );
	else if( level == ATS_LOG_WARNING ) env->warnings.push_back( msg );
}

// 確保数を数えるアロケータ（Env の破棄時にリークを検出する）
static void* ATS_CALL CountingAlloc( size_t size, size_t, void* user )
{
	++static_cast<Env*>( user )->liveAllocs;
	return std::malloc( size ? size : 1 );
}

static void ATS_CALL CountingFree( void* p, void* user )
{
	if( !p ) return;
	--static_cast<Env*>( user )->liveAllocs;
	std::free( p );
}

Env::Env()
{
	ats_runtime_desc d{};
	d.size  = sizeof(d);
	d.alloc = CountingAlloc;
	d.free  = CountingFree;
	d.log   = LogFn;
	d.user  = this;
	ats_runtime_create( &d, &rt );
}

Env::~Env()
{
	ats_vm_destroy( vm );
	ats_runtime_destroy( rt );
	if( liveAllocs != 0 ){
		std::printf( "  memory leak: %ld allocation(s) not freed\n", liveAllocs );
		++g_failures;
	}
}

ats_vm* Env::MakeVm( uint32_t budget, uint64_t seed )
{
	ats_vm_destroy( vm );
	vm = nullptr;
	ats_vm_desc d{};
	d.size               = sizeof(d);
	d.instruction_budget = budget;
	d.random_seed        = seed;
	ats_vm_create( rt, &d, &vm );
	return vm;
}

ats_result Env::LoadResult( ats::writer::ProgramWriter& w, ats_program** out )
{
	std::string err;
	std::vector<uint8_t> bytes = w.Build( &err );
	if( bytes.empty() ){
		std::printf( "  build error: %s\n", err.c_str() );
		*out = nullptr;
		return ATS_ERR_BAD_FORMAT;
	}
	return ats_program_load( rt, bytes.data(), bytes.size(), out );
}

ats_program* Env::Load( ats::writer::ProgramWriter& w )
{
	ats_program* p = nullptr;
	ats_result r = LoadResult( w, &p );
	if( r != ATS_OK ) std::printf( "  load error: %s (%s)\n", ats_result_string( r ), ats_runtime_last_error( rt ) );
	return p;
}

void Env::Update( float dt, int times )
{
	for( int i = 0; i < times; ++i ) ats_vm_update( vm, dt );
}

void Env::Fire( ats_program* prog, const char* event, std::vector<ats_value> args )
{
	ats_result r = ats_vm_fire_event( vm, prog, event, args.data(), (int)args.size(), nullptr );
	if( r != ATS_OK ) std::printf( "  fire error: %s (%s)\n", ats_result_string( r ), ats_vm_last_error( vm ) );
}

static ats_status ATS_CALL ReportFn( ats_call* call, void* user )
{
	Env* env = static_cast<Env*>( user );
	ats_value v = ats_call_arg( call, 0 );
	char buf[64];
	switch( v.type ){
	case ATS_TYPE_FLOAT:	std::snprintf( buf, sizeof(buf), "%g", v.v.f ); break;
	case ATS_TYPE_STRING:	std::snprintf( buf, sizeof(buf), "%s", v.v.s ); break;
	case ATS_TYPE_BOOL:		std::snprintf( buf, sizeof(buf), "%s", v.v.b ? "true" : "false" ); break;
	default:				std::snprintf( buf, sizeof(buf), "%d", v.v.i ); break;
	}
	env->reports.push_back( buf );
	return ATS_DONE;
}

void Env::RegisterReporters()
{
	const char* names[] = { "Report", "ReportF", "ReportS", "ReportB" };
	for( const char* n : names ){
		ats_command_desc d{};
		d.size = sizeof(d);
		d.name = n;
		d.fn   = ReportFn;
		d.user = this;
		ats_register_command( rt, &d );
	}
}

std::string Env::Reports() const
{
	std::string s;
	for( size_t i = 0; i < reports.size(); ++i ){
		if( i ) s += ",";
		s += reports[i];
	}
	return s;
}

//=========================================================================
// メモリ上のセーブデータ
//=========================================================================
ats_result ATS_CALL MemoryFile::Write( const void* p, size_t n, void* user )
{
	MemoryFile* f = static_cast<MemoryFile*>( user );
	const uint8_t* b = static_cast<const uint8_t*>( p );
	f->data.insert( f->data.end(), b, b + n );
	return ATS_OK;
}

ats_result ATS_CALL MemoryFile::Read( void* p, size_t n, void* user )
{
	MemoryFile* f = static_cast<MemoryFile*>( user );
	if( f->pos + n > f->data.size() ) return ATS_ERR_BAD_FORMAT;
	std::memcpy( p, f->data.data() + f->pos, n );
	f->pos += n;
	return ATS_OK;
}

//=========================================================================
// 実行
//=========================================================================
int main()
{
	int failedTests = 0;
	for( const TestCase& t : AllTests() ){
		int before = g_failures;
		t.fn();
		bool ok = g_failures == before;
		if( !ok ) ++failedTests;
		std::printf( "[%s] %s\n", ok ? " OK " : "FAIL", t.name );
	}
	std::printf( "\n%zu tests, %d failed\n", AllTests().size(), failedTests );
	return failedTests ? 1 : 0;
}
