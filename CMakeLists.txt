cmake_minimum_required( VERSION 3.16 )
project( AtomScript VERSION 0.1.0 LANGUAGES CXX )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

option( ATS_BUILD_SHARED	"共有ライブラリ（DLL / .so / .dylib）を作る"	ON )
option( ATS_BUILD_TESTS		"テストを作る"									ON )
option( ATS_ENABLE_DEBUG	"デバッグ用 API を含める（製品ビルドでは OFF）"	ON )

# MSVC の既定の /EHsc を外し、ターゲットごとに指定する
if( MSVC )
	string( REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" )
endif()

set( ATS_CORE_SOURCES
	src/ats_internal.h
	src/ats_runtime.cpp
	src/ats_program.cpp
	src/ats_vm.cpp
	src/ats_vars.cpp
	include/atomscript/ats_api.h
	include/atomscript/ats_format.h
)

# コアは例外・RTTI を使わない（家庭用機・UE の既定設定に合わせる）
function( ats_core_options target )
	target_include_directories( ${target} PUBLIC include )
	if( MSVC )
		target_compile_options( ${target} PRIVATE /W4 /GR- /EHs-c- /utf-8 )
		target_compile_definitions( ${target} PRIVATE _HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS )
	else()
		target_compile_options( ${target} PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti -fvisibility=hidden )
	endif()
	if( ATS_ENABLE_DEBUG )
		target_compile_definitions( ${target} PUBLIC ATS_ENABLE_DEBUG )
	endif()
endfunction()

# 静的ライブラリ（iOS・家庭用機・テスト）
add_library( atomscript_static STATIC ${ATS_CORE_SOURCES} )
ats_core_options( atomscript_static )
target_compile_definitions( atomscript_static PUBLIC ATS_STATIC )
set_target_properties( atomscript_static PROPERTIES OUTPUT_NAME atomscript_static )

# 共有ライブラリ（Windows / macOS / Android / Linux）
if( ATS_BUILD_SHARED )
	add_library( atomscript SHARED ${ATS_CORE_SOURCES} )
	ats_core_options( atomscript )
	target_compile_definitions( atomscript PRIVATE ATS_BUILD_DLL )
endif()

# .atsb の書き出し（コンパイラ・テスト用。STL を使う）
add_library( atomscript_writer STATIC tools/writer/ats_writer.cpp tools/writer/ats_writer.h )
target_include_directories( atomscript_writer PUBLIC include tools/writer )
if( MSVC )
	target_compile_options( atomscript_writer PRIVATE /W4 /utf-8 /EHsc )
else()
	target_compile_options( atomscript_writer PRIVATE -Wall -Wextra )
endif()

# コンパイラ（.ats → .atsb）。CLI・Unity・UE・エディタが同じ実装を使う
add_library( atomscript_compiler STATIC
	tools/compiler/ats_diag.h
	tools/compiler/ats_diag.cpp
	tools/compiler/ats_yaml.h
	tools/compiler/ats_yaml.cpp
	tools/compiler/ats_manifest.h
	tools/compiler/ats_manifest.cpp
	tools/compiler/ats_lexer.h
	tools/compiler/ats_lexer.cpp
	tools/compiler/ats_ast.h
	tools/compiler/ats_parser.h
	tools/compiler/ats_parser.cpp
	tools/compiler/ats_compiler.h
	tools/compiler/ats_compiler.cpp
	tools/compiler/ats_disasm.cpp
	tools/compiler/ats_gen.cpp
	tools/compiler/ats_json.h
	tools/compiler/ats_json.cpp
	tools/compiler/ats_lsp.h
	tools/compiler/ats_lsp.cpp
)
target_include_directories( atomscript_compiler PUBLIC tools/compiler )
target_link_libraries( atomscript_compiler PUBLIC atomscript_writer )
if( MSVC )
	target_compile_options( atomscript_compiler PRIVATE /W4 /utf-8 /EHsc )
else()
	target_compile_options( atomscript_compiler PRIVATE -Wall -Wextra )
endif()

add_executable( atsc tools/compiler/atsc_main.cpp )
target_link_libraries( atsc PRIVATE atomscript_compiler )
if( MSVC )
	target_compile_options( atsc PRIVATE /W4 /utf-8 /EHsc )
endif()

if( ATS_BUILD_TESTS )
	enable_testing()

	# サンプルのマニフェストから登録コードを生成し、テストでそのまま使う（atsc gen の検証）
	set( ATS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated" )
	file( MAKE_DIRECTORY "${ATS_GENERATED_DIR}" )
	add_custom_command(
		OUTPUT "${ATS_GENERATED_DIR}/sample_rpg.atsgen.h"
		COMMAND atsc gen -m "${CMAKE_CURRENT_SOURCE_DIR}/samples/sample.atsmanifest.yaml" --lang cpp -o "${ATS_GENERATED_DIR}/sample_rpg.atsgen.h"
		DEPENDS atsc "${CMAKE_CURRENT_SOURCE_DIR}/samples/sample.atsmanifest.yaml"
		COMMENT "atsc gen: samples/sample.atsmanifest.yaml"
	)

	add_executable( atomscript_tests
		tests/test_framework.h
		tests/test_main.cpp
		tests/test_basic.cpp
		tests/test_commands.cpp
		tests/test_fibers.cpp
		tests/test_vars.cpp
		tests/test_compiler.cpp
		tests/test_gen.cpp
		tests/test_lsp.cpp
		"${ATS_GENERATED_DIR}/sample_rpg.atsgen.h"
	)
	target_include_directories( atomscript_tests PRIVATE "${ATS_GENERATED_DIR}" )
	target_link_libraries( atomscript_tests PRIVATE atomscript_static atomscript_writer atomscript_compiler )
	target_compile_definitions( atomscript_tests PRIVATE ATS_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples" )
	if( MSVC )
		target_compile_options( atomscript_tests PRIVATE /W4 /utf-8 /EHsc )
	else()
		target_compile_options( atomscript_tests PRIVATE -Wall -Wextra )
	endif()
	add_test( NAME atomscript_tests COMMAND atomscript_tests )
endif()

# Unity パッケージ：テスト用の生成コード・バイトコードと、Windows 用のネイティブプラグイン
# （パッケージのフォルダーに直接書き出す。生成物はリポジトリに入れ、atsc gen の出力の変化を差分で見られるようにする）
set( ATS_UNITY_PACKAGE "${CMAKE_CURRENT_SOURCE_DIR}/unity/com.pupooh15.atomscript" )
if( EXISTS "${ATS_UNITY_PACKAGE}" )
	set( ATS_UNITY_TESTS "${ATS_UNITY_PACKAGE}/Tests/Editor" )
	set( ATS_UNITY_OUTPUTS )
	# ats_unity_asset( 名前 マニフェスト スクリプト )
	function( ats_unity_asset name manifest script )
		add_custom_command(
			OUTPUT "${ATS_UNITY_TESTS}/Generated/${name}.g.cs" "${ATS_UNITY_TESTS}/Data/${name}.atsb.bytes"
			COMMAND atsc gen -m "${manifest}" --lang csharp -o "${ATS_UNITY_TESTS}/Generated/${name}.g.cs"
			COMMAND atsc compile "${script}" -m "${manifest}" -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.atsb"
			COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/${name}.atsb" "${ATS_UNITY_TESTS}/Data/${name}.atsb.bytes"
			DEPENDS atsc "${manifest}" "${script}"
			COMMENT "Unity テスト用アセット: ${name}"
		)
		set( ATS_UNITY_OUTPUTS ${ATS_UNITY_OUTPUTS} "${ATS_UNITY_TESTS}/Generated/${name}.g.cs" "${ATS_UNITY_TESTS}/Data/${name}.atsb.bytes" PARENT_SCOPE )
	endfunction()
	ats_unity_asset( SampleRpg "${CMAKE_CURRENT_SOURCE_DIR}/samples/sample.atsmanifest.yaml" "${CMAKE_CURRENT_SOURCE_DIR}/samples/merchant.ats" )
	ats_unity_asset( WrapperTest "${ATS_UNITY_TESTS}/Data/wrapper_test.atsmanifest.yaml" "${ATS_UNITY_TESTS}/Data/wrapper_test.ats" )
	add_custom_target( unity_test_assets ALL DEPENDS ${ATS_UNITY_OUTPUTS} )

	# エディタ（Windows x64）で使う DLL をパッケージにコピーする（最後にビルドした構成のものが入る）
	if( WIN32 AND ATS_BUILD_SHARED AND CMAKE_SIZEOF_VOID_P EQUAL 8 )
		add_custom_target( unity_plugin ALL
			COMMAND ${CMAKE_COMMAND} -E make_directory "${ATS_UNITY_PACKAGE}/Runtime/Plugins/Windows/x86_64"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:atomscript>" "${ATS_UNITY_PACKAGE}/Runtime/Plugins/Windows/x86_64/"
			DEPENDS atomscript
			COMMENT "atomscript.dll を Unity パッケージにコピー"
		)
	endif()
endif()
