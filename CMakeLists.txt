cmake_minimum_required( VERSION 3.16 )

# macOS は Apple Silicon・Intel の両方で動くユニバーサルバイナリにする（Unity 6 の動作環境に合わせて 11.0 以上）。
# project() より前に決める必要がある
if( APPLE AND NOT CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_SYSTEM_NAME MATCHES "iOS" )
	set( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS のアーキテクチャ" )
endif()
if( APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_SYSTEM_NAME MATCHES "iOS" )
	set( CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS の最小バージョン" )
endif()

project( AtomScript VERSION 0.1.0 LANGUAGES CXX )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

option( ATS_BUILD_SHARED	"共有ライブラリ（DLL / .so / .dylib）を作る"	ON )
option( ATS_BUILD_TESTS		"テストを作る"									ON )
option( ATS_ENABLE_DEBUG	"デバッグ用 API を含める（製品ビルドでは OFF）"	ON )

# ツール（writer・コンパイラ・atsc・テスト）はホストで動くものなので、Android・iOS へのクロスビルドでは作らない
if( CMAKE_CROSSCOMPILING )
	set( ATS_BUILD_TOOLS_DEFAULT OFF )
else()
	set( ATS_BUILD_TOOLS_DEFAULT ON )
endif()
option( ATS_BUILD_TOOLS		"ツール（atsc）とテストを作る"					${ATS_BUILD_TOOLS_DEFAULT} )

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

	# Android：16KB ページの端末でも読めるようにする（Google Play が Android 15 以降向けに必須にしている）
	if( ANDROID )
		target_link_options( atomscript PRIVATE "-Wl,-z,max-page-size=16384" )
	endif()

	# macOS の Unity 用プラグイン（atomscript.bundle）。DllImport( "atomscript" ) で読まれる
	if( APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES "iOS" )
		add_library( atomscript_bundle MODULE ${ATS_CORE_SOURCES} )
		ats_core_options( atomscript_bundle )
		target_compile_definitions( atomscript_bundle PRIVATE ATS_BUILD_DLL )
		set_target_properties( atomscript_bundle PROPERTIES
			BUNDLE						TRUE
			OUTPUT_NAME					atomscript
			MACOSX_BUNDLE_GUI_IDENTIFIER	com.pupooh15.atomscript
			MACOSX_BUNDLE_BUNDLE_NAME		atomscript
			MACOSX_BUNDLE_BUNDLE_VERSION	${PROJECT_VERSION}
			MACOSX_BUNDLE_SHORT_VERSION_STRING	${PROJECT_VERSION}
		)
	endif()
endif()

if( ATS_BUILD_TOOLS )

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

endif()	# ATS_BUILD_TOOLS

if( ATS_BUILD_TESTS AND ATS_BUILD_TOOLS )
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
	target_link_libraries( atomscript_tests PRIVATE atomscript_static atomscript_compiler )
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
	set( ATS_UNITY_RUNTIME_TESTS "${ATS_UNITY_PACKAGE}/Tests/Runtime" )
	set( ATS_UNITY_OUTPUTS )
	if( ATS_BUILD_TOOLS )		# テスト用アセットは atsc で作る（クロスビルドでは作らない）
	# ats_unity_asset( 名前 マニフェスト スクリプト 生成コードの置き場所 )
	function( ats_unity_asset name manifest script gen_dir )
		add_custom_command(
			OUTPUT "${gen_dir}/${name}.g.cs" "${ATS_UNITY_TESTS}/Data~/${name}.atsb.bytes"
			COMMAND atsc gen -m "${manifest}" --lang csharp -o "${gen_dir}/${name}.g.cs"
			COMMAND atsc compile "${script}" -m "${manifest}" -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.atsb"
			COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/${name}.atsb" "${ATS_UNITY_TESTS}/Data~/${name}.atsb.bytes"
			DEPENDS atsc "${manifest}" "${script}"
			COMMENT "Unity テスト用アセット: ${name}"
		)
		set( ATS_UNITY_OUTPUTS ${ATS_UNITY_OUTPUTS} "${gen_dir}/${name}.g.cs" "${ATS_UNITY_TESTS}/Data~/${name}.atsb.bytes" PARENT_SCOPE )
	endfunction()
	# SampleRpg はプレイヤー用テスト（Tests/Runtime）でも使うので、生成コードはそちらに置く
	ats_unity_asset( SampleRpg "${CMAKE_CURRENT_SOURCE_DIR}/samples/sample.atsmanifest.yaml" "${CMAKE_CURRENT_SOURCE_DIR}/samples/merchant.ats" "${ATS_UNITY_RUNTIME_TESTS}/Generated" )
	ats_unity_asset( WrapperTest "${ATS_UNITY_TESTS}/Data~/wrapper_test.atsmanifest.yaml" "${ATS_UNITY_TESTS}/Data~/wrapper_test.ats" "${ATS_UNITY_TESTS}/Generated" )

	# プレイヤーではファイルを読めないので、.atsb を C# のバイト配列にして埋め込む
	add_custom_command(
		OUTPUT "${ATS_UNITY_RUNTIME_TESTS}/Generated/SampleRpgBytecode.g.cs"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${ATS_UNITY_TESTS}/Data~/SampleRpg.atsb.bytes -DOUTPUT=${ATS_UNITY_RUNTIME_TESTS}/Generated/SampleRpgBytecode.g.cs
			-DNAMESPACE=AtomScript.Tests -DCLASS=SampleRpgBytecode -DSOURCE=samples/merchant.ats -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/embed_csharp_bytes.cmake"
		DEPENDS "${ATS_UNITY_TESTS}/Data~/SampleRpg.atsb.bytes" "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/embed_csharp_bytes.cmake"
		COMMENT "Unity テスト用アセット: SampleRpgBytecode.g.cs"
	)
	list( APPEND ATS_UNITY_OUTPUTS "${ATS_UNITY_RUNTIME_TESTS}/Generated/SampleRpgBytecode.g.cs" )
	add_custom_target( unity_test_assets ALL DEPENDS ${ATS_UNITY_OUTPUTS} )
	endif()	# ATS_BUILD_TOOLS

	# プラグインの .meta（GUID とプラットフォーム設定）は unity/plugin-meta に置き、バイナリと一緒にコピーする。
	# パッケージの Runtime/Plugins はビルドで作るもので、リポジトリには入れない
	# （入れておくと、バイナリの無いプラットフォームで Unity が持ち主のいない .meta を消してしまう）
	set( ATS_UNITY_PLUGIN_META "${CMAKE_CURRENT_SOURCE_DIR}/unity/plugin-meta" )
	set( ATS_UNITY_PLUGINS "${ATS_UNITY_PACKAGE}/Runtime/Plugins" )

	# エディタ（Windows x64）で使う DLL と、インポーターが呼ぶ atsc をパッケージにコピーする（最後にビルドした構成のものが入る）
	if( WIN32 AND ATS_BUILD_SHARED AND CMAKE_SIZEOF_VOID_P EQUAL 8 )
		add_custom_target( unity_plugin ALL
			COMMAND ${CMAKE_COMMAND} -E make_directory "${ATS_UNITY_PLUGINS}/Windows/x86_64"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_UNITY_PLUGIN_META}/Plugins.meta" "${ATS_UNITY_PACKAGE}/Runtime/"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_UNITY_PLUGIN_META}/Plugins/Windows.meta" "${ATS_UNITY_PLUGINS}/"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_UNITY_PLUGIN_META}/Plugins/Windows/x86_64.meta" "${ATS_UNITY_PLUGINS}/Windows/"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_UNITY_PLUGIN_META}/Plugins/Windows/x86_64/atomscript.dll.meta" "${ATS_UNITY_PLUGINS}/Windows/x86_64/"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:atomscript>" "${ATS_UNITY_PLUGINS}/Windows/x86_64/"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${ATS_UNITY_PACKAGE}/Editor/Tools~/win-x64"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:atsc>" "${ATS_UNITY_PACKAGE}/Editor/Tools~/win-x64/"
			DEPENDS atomscript atsc
			COMMENT "atomscript.dll と atsc.exe を Unity パッケージにコピー"
		)
	endif()

	# macOS（ユニバーサル）：atomscript.bundle と atsc をコピーする。bundle は中身ごと置き換える
	if( APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES "iOS" AND ATS_BUILD_SHARED )
		add_custom_target( unity_plugin ALL
			COMMAND ${CMAKE_COMMAND} -E make_directory "${ATS_UNITY_PLUGINS}/macOS"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_UNITY_PLUGIN_META}/Plugins.meta" "${ATS_UNITY_PACKAGE}/Runtime/"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_UNITY_PLUGIN_META}/Plugins/macOS.meta" "${ATS_UNITY_PLUGINS}/"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_UNITY_PLUGIN_META}/Plugins/macOS/atomscript.bundle.meta" "${ATS_UNITY_PLUGINS}/macOS/"
			COMMAND ${CMAKE_COMMAND} -E rm -rf "${ATS_UNITY_PLUGINS}/macOS/atomscript.bundle"
			COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:atomscript_bundle>" "${ATS_UNITY_PLUGINS}/macOS/atomscript.bundle"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${ATS_UNITY_PACKAGE}/Editor/Tools~/osx"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:atsc>" "${ATS_UNITY_PACKAGE}/Editor/Tools~/osx/"
			DEPENDS atomscript_bundle atsc
			COMMENT "atomscript.bundle と atsc を Unity パッケージにコピー"
		)
	endif()

	# IL2CPP の機種（Android・iOS・家庭用機）：コアのソースをパッケージに置き、ゲームのビルド時に IL2CPP がコンパイルする。
	# 機種ごとのバイナリは作らない（エディタ・デスクトップは上のビルド済みバイナリ）。ホストのビルドならどの OS でもコピーする
	if( NOT CMAKE_CROSSCOMPILING )
		set( ATS_IL2CPP_DIR "${ATS_UNITY_PLUGINS}/IL2CPP/AtomScript" )
		set( ATS_IL2CPP_META "${ATS_UNITY_PLUGIN_META}/Plugins/IL2CPP" )
		# IL2CPP はソースを平らにコピーしてコンパイルするので、サブフォルダーを作らず 1 つのフォルダーに並べる
		set( ATS_IL2CPP_COMMANDS
			COMMAND ${CMAKE_COMMAND} -E make_directory "${ATS_IL2CPP_DIR}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_UNITY_PLUGIN_META}/Plugins.meta" "${ATS_UNITY_PACKAGE}/Runtime/"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_UNITY_PLUGIN_META}/Plugins/IL2CPP.meta" "${ATS_UNITY_PLUGINS}/"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_IL2CPP_META}/AtomScript.meta" "${ATS_UNITY_PLUGINS}/IL2CPP/"
		)
		set( ATS_IL2CPP_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/copy_il2cpp_source.cmake" )
		foreach( f src/ats_internal.h src/ats_runtime.cpp src/ats_program.cpp src/ats_vm.cpp src/ats_vars.cpp
				   include/atomscript/ats_api.h include/atomscript/ats_format.h )
			get_filename_component( name "${f}" NAME )
			if( NOT EXISTS "${ATS_IL2CPP_META}/AtomScript/${name}.meta" )
				message( FATAL_ERROR "IL2CPP 用の .meta がありません: unity/plugin-meta/Plugins/IL2CPP/AtomScript/${name}.meta" )
			endif()
			list( APPEND ATS_IL2CPP_COMMANDS
				COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/${f} -DOUTPUT=${ATS_IL2CPP_DIR}/${name}
					-P "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/copy_il2cpp_source.cmake"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ATS_IL2CPP_META}/AtomScript/${name}.meta" "${ATS_IL2CPP_DIR}/"
			)
			list( APPEND ATS_IL2CPP_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${f}" )
		endforeach()
		add_custom_target( unity_il2cpp_source ALL ${ATS_IL2CPP_COMMANDS}
			DEPENDS ${ATS_IL2CPP_DEPENDS}
			COMMENT "コアのソースを Unity パッケージ（IL2CPP 用）にコピー"
		)
	endif()
endif()
