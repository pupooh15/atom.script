/**************************************************************************/
/*!	\file	test_gen.cpp
	\brief	atsc gen の生成コード（ビルド時に samples のマニフェストから生成）を使うテスト
***************************************************************************/
#include "test_framework.h"
#include "ats_compiler.h"
#include "sample_rpg.atsgen.h"

#include <fstream>
#include <sstream>

using namespace ats::compiler;

namespace {

// ゲーム側の実装（生成された sample_rpg::Commands を実装する）
class SampleCommands : public sample_rpg::Commands {
public:
	std::vector<std::string>	log;
	std::vector<int32_t>		answers;		// SelectWindow の答え（-1 なら PENDING で待つ）
	ats_call_token				pending = 0;
	bool						questCleared = false;
	std::vector<ats_call_token>	cancelled;

	ats_status ShowMessage( ats_call*, const char* text, sample_rpg::Face face ) override
	{
		log.push_back( std::string( "msg:" ) + text + ":" + std::to_string( static_cast<int>( face ) ) );
		return ATS_DONE;
	}
	ats_status SelectWindow( ats_call* call, sample_rpg::SelectType type ) override
	{
		log.push_back( "select:" + std::to_string( static_cast<int>( type ) ) );
		int32_t a = answers.empty() ? 0 : answers.front();
		if( !answers.empty() ) answers.erase( answers.begin() );
		if( a < 0 ){
			pending = ats_call_get_token( call );
			return ATS_PENDING;
		}
		const ats_value v = sample_rpg::MakeValue( a );
		ats_call_set_result( call, &v );
		return ATS_DONE;
	}
	void CameraShake( float power, float duration ) override
	{
		char buf[64];
		snprintf( buf, sizeof(buf), "shake:%g:%g", power, duration );
		log.push_back( buf );
	}
	ats_status FadeOut( ats_call*, float time ) override
	{
		char buf[32];
		snprintf( buf, sizeof(buf), "fade:%g", time );
		log.push_back( buf );
		return ATS_DONE;
	}
	ats_status PlayBgm( ats_call*, const char* name ) override
	{
		log.push_back( std::string( "bgm:" ) + name );
		return ATS_DONE;
	}
	bool IsQuestCleared( int32_t quest_id ) override
	{
		log.push_back( "cleared?" + std::to_string( quest_id ) );
		return questCleared;
	}
	void OnCancel( ats_call_token token ) override { cancelled.push_back( token ); }

	std::string Log() const
	{
		std::string s;
		for( size_t i = 0; i < log.size(); ++i ){ if( i ) s += ","; s += log[i]; }
		return s;
	}
};

std::string ReadText( const std::string& path )
{
	std::ifstream f( path, std::ios::binary );
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// マニフェストとサンプルスクリプトを読み込んでコンパイルする
bool CompileMerchant( Manifest* m, CompileResult* r )
{
	Diagnostics d;
	std::string dir = ATS_SAMPLES_DIR;
	if( !m->Load( dir + "/sample.atsmanifest.yaml", d ) ){ std::printf( "%s", d.ToText().c_str() ); return false; }
	if( !CompileSource( ReadText( dir + "/merchant.ats" ), "merchant.ats", *m, d, r ) ){ std::printf( "%s", d.ToText().c_str() ); return false; }
	return true;
}

}	// namespace

TEST( GeneratedCodeRunsMerchant )
{
	Env env;
	SampleCommands impl;
	REQUIRE( sample_rpg::Register( env.rt, &impl ) == ATS_OK );
	env.MakeVm();

	Manifest m;
	CompileResult r;
	REQUIRE( CompileMerchant( &m, &r ) );

	// 生成コードとコンパイル結果は同じマニフェストから作られている
	ats::fmt::FileHeader h;
	std::memcpy( &h, r.bytes.data(), sizeof(h) );
	CHECK_EQ( h.manifest_hash, sample_rpg::kManifestHash );

	// シグネチャのハッシュが一致しないと、ここで読み込みに失敗する
	ats_program* prog = nullptr;
	REQUIRE( ats_program_load( env.rt, r.bytes.data(), r.bytes.size(), &prog ) == ATS_OK );

	// 1 回目の確認は保留（PENDING）して、ホストから後で答える
	impl.answers = { -1, 0 };
	CHECK_EQ( sample_rpg::events::FireOnTalk( env.vm, prog, 42 ), ATS_OK );
	env.Update( 0.1f );
	CHECK_EQ( impl.Log(), std::string( "msg:また来てくれ。:0,select:0" ) );
	REQUIRE( impl.pending != 0 );

	const ats_value no = sample_rpg::MakeValue( 1 );
	CHECK_EQ( ats_call_complete( env.vm, impl.pending, &no ), ATS_OK );
	for( int i = 0; i < 20; ++i ) env.Update( 0.1f );		// wait 1.5 を越える
	CHECK_EQ( impl.Log(), std::string( "msg:また来てくれ。:0,select:0,msg:本当に？:0,select:0,fade:0.5,bgm:bgm_shop" ) );

	ats_value v;
	CHECK_EQ( ats_var_get( env.vm, sample_rpg::vars::scene::reward_count, &v ), ATS_OK );
	CHECK_EQ( v.v.i, 1 );
	CHECK_EQ( ats_vm_fiber_count( env.vm ), 0 );
	CHECK( env.errors.empty() );
	ats_program_release( prog );
}

TEST( GeneratedCodeSharedVarsAndQueries )
{
	Env env;
	SampleCommands impl;
	REQUIRE( sample_rpg::Register( env.rt, &impl ) == ATS_OK );
	env.MakeVm();

	Manifest m;
	CompileResult r;
	REQUIRE( CompileMerchant( &m, &r ) );
	ats_program* prog = nullptr;
	REQUIRE( ats_program_load( env.rt, r.bytes.data(), r.bytes.size(), &prog ) == ATS_OK );

	// 3 章ならクエストを確かめ、未クリアならスマイルで話してカメラを揺らす
	const ats_value chapter = sample_rpg::MakeValue( 3 );
	CHECK_EQ( ats_var_set( env.vm, sample_rpg::vars::story::chapter, &chapter ), ATS_OK );
	impl.answers = { 0 };
	CHECK_EQ( sample_rpg::events::BroadcastOnTalk( env.vm, 1 ), ATS_OK );	// まだ結び付いていないので 0 本
	CHECK_EQ( sample_rpg::events::FireOnTalk( env.vm, prog, 1 ), ATS_OK );
	env.Update( 0.1f );
	CHECK_EQ( impl.Log(), std::string( "cleared?12,msg:例の品は手に入ったかい？:1,shake:2:0.5,select:0,fade:0.5,bgm:bgm_shop" ) );

	ats_value met;
	CHECK_EQ( ats_var_get( env.vm, sample_rpg::vars::story::met_merchant, &met ), ATS_OK );
	CHECK_EQ( met.v.b, 1 );

	// 中断すると待機中のコマンドに OnCancel が届く（wait 中なので届かない）
	ats_vm_destroy( env.vm );
	env.vm = nullptr;
	CHECK( impl.cancelled.empty() );
	ats_program_release( prog );
}

TEST( GeneratedCodeCancel )
{
	Env env;
	SampleCommands impl;
	REQUIRE( sample_rpg::Register( env.rt, &impl ) == ATS_OK );
	env.MakeVm();

	Manifest m;
	CompileResult r;
	REQUIRE( CompileMerchant( &m, &r ) );
	ats_program* prog = nullptr;
	REQUIRE( ats_program_load( env.rt, r.bytes.data(), r.bytes.size(), &prog ) == ATS_OK );

	impl.answers = { -1 };
	ats_fiber_id fiber = 0;
	CHECK_EQ( sample_rpg::events::FireOnTalk( env.vm, prog, 1, &fiber ), ATS_OK );
	env.Update( 0.1f );
	REQUIRE( impl.pending != 0 );
	CHECK_EQ( ats_vm_abort_fiber( env.vm, fiber ), ATS_OK );
	REQUIRE( impl.cancelled.size() == 1 );
	CHECK_EQ( impl.cancelled[0], impl.pending );
	ats_program_release( prog );
}

TEST( GenerateHtmlAndDeterminism )
{
	Manifest m;
	Diagnostics d;
	REQUIRE( m.Load( std::string( ATS_SAMPLES_DIR ) + "/sample.atsmanifest.yaml", d ) );
	GenOptions opt;
	opt.source = "sample.atsmanifest.yaml";
	std::string html = GenerateHtml( m, opt );
	CHECK( html.find( "メッセージ表示" ) != std::string::npos );
	CHECK( html.find( "await ShowMessage(text: string, face: Face = Normal)" ) != std::string::npos );
	CHECK( html.find( "story.chapter" ) != std::string::npos );
	CHECK_EQ( GenerateCpp( m, opt ), GenerateCpp( m, opt ) );

	// 名前空間の指定
	opt.nameSpace = "game::script";
	CHECK( GenerateCpp( m, opt ).find( "namespace game::script {" ) != std::string::npos );
}
