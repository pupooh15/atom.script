/**************************************************************************/
/*!	\file	test_framework.h
	\brief	最小限のテストフレームワークとテスト用の環境
***************************************************************************/
#ifndef ATS_TEST_FRAMEWORK_H
#define ATS_TEST_FRAMEWORK_H

#include "atomscript/ats_api.h"
#include "ats_writer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

//=========================================================================
// テストの登録・判定
//=========================================================================
struct TestCase { const char* name; void (*fn)(); };
std::vector<TestCase>& AllTests();
struct TestRegistrar { TestRegistrar( const char* n, void (*f)() ) { AllTests().push_back( { n, f } ); } };
extern int g_failures;

#define TEST( name ) \
	static void name(); \
	static TestRegistrar s_reg_##name( #name, name ); \
	static void name()

#define CHECK( cond ) \
	do { if( !(cond) ){ std::printf( "  %s(%d): CHECK( %s ) failed\n", __FILE__, __LINE__, #cond ); ++g_failures; } } while( 0 )

#define CHECK_EQ( a, b ) \
	do { auto va_ = (a); auto vb_ = (b); if( !(va_ == vb_) ){ \
		std::printf( "  %s(%d): CHECK_EQ( %s, %s ) failed\n", __FILE__, __LINE__, #a, #b ); ++g_failures; } } while( 0 )

#define REQUIRE( cond ) \
	do { if( !(cond) ){ std::printf( "  %s(%d): REQUIRE( %s ) failed\n", __FILE__, __LINE__, #cond ); ++g_failures; return; } } while( 0 )

//=========================================================================
// テスト用の環境（ログの収集、記録用コマンド）
//=========================================================================
struct Env {
	ats_runtime*				rt = nullptr;
	ats_vm*						vm = nullptr;
	std::vector<std::string>	errors;
	std::vector<std::string>	warnings;
	std::vector<std::string>	reports;		// Report 系コマンドの記録
	long						liveAllocs = 0;	// 解放されていない確保の数

	Env();
	~Env();

	// VM を（作り直して）用意する。コマンド登録の後に呼ぶ
	ats_vm*			MakeVm( uint32_t budget = 0, uint64_t seed = 0 );
	ats_program*	Load( ats::writer::ProgramWriter& w );
	ats_result		LoadResult( ats::writer::ProgramWriter& w, ats_program** out );
	void			Update( float dt = 1.0f / 60.0f, int times = 1 );
	void			Fire( ats_program* prog, const char* event, std::vector<ats_value> args = {} );

	// Report(int) / ReportF(float) / ReportS(string) を登録する
	void			RegisterReporters();
	std::string		Reports() const;
};

ats_value V_Int( int32_t v );
ats_value V_Float( float v );
ats_value V_Bool( bool v );
ats_value V_Str( const char* s );
ats_value V_Handle( uint64_t h );

// メモリ上のセーブデータ
struct MemoryFile {
	std::vector<uint8_t>	data;
	size_t					pos = 0;
	static ats_result ATS_CALL Write( const void* p, size_t n, void* user );
	static ats_result ATS_CALL Read( void* p, size_t n, void* user );
};

#endif	// ATS_TEST_FRAMEWORK_H
