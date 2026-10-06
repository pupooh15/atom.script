/**************************************************************************/
/*!	\file	atsc_main.cpp
	\brief	AtomScript コンパイラ CLI（atsc）
***************************************************************************/
#include "ats_compiler.h"
#include "ats_lsp.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#endif

using namespace ats::compiler;

namespace {

const char* kUsage =
	"AtomScript compiler\n"
	"\n"
	"usage:\n"
	"  atsc compile  <file.ats>... -m <manifest> [-o <out.atsb | dir>] [--json] [--no-debug]\n"
	"  atsc validate <file.ats>... -m <manifest> [--json]\n"
	"  atsc disasm   <file.atsb>\n"
	"  atsc gen      -m <manifest> --lang <cpp | html> [-o <file>] [--namespace <ns>]\n"
	"  atsc lsp      （言語サーバー。標準入出力で通信する）\n"
	"  atsc --version\n";

bool ReadFile( const std::string& path, std::string* out )
{
	std::ifstream f( path, std::ios::binary );
	if( !f ) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	*out = ss.str();
	return true;
}

bool WriteFile( const std::string& path, const std::vector<uint8_t>& data )
{
	std::ofstream f( path, std::ios::binary );
	if( !f ) return false;
	f.write( reinterpret_cast<const char*>( data.data() ), (std::streamsize)data.size() );
	return (bool)f;
}

std::string ReplaceExt( const std::string& path, const std::string& ext )
{
	size_t slash = path.find_last_of( "/\\" );
	size_t dot = path.find_last_of( '.' );
	if( dot == std::string::npos || (slash != std::string::npos && dot < slash) ) return path + ext;
	return path.substr( 0, dot ) + ext;
}

std::string FileName( const std::string& path )
{
	size_t slash = path.find_last_of( "/\\" );
	return slash == std::string::npos ? path : path.substr( slash + 1 );
}

bool EndsWith( const std::string& s, const std::string& suffix )
{
	return s.size() >= suffix.size() && s.compare( s.size() - suffix.size(), suffix.size(), suffix ) == 0;
}

void Print( const Diagnostics& diag, bool json )
{
	if( json ) std::printf( "%s\n", diag.ToJson().c_str() );
	else std::fputs( diag.ToText().c_str(), stderr );
}

int Disasm( const std::string& path )
{
	std::string data;
	if( !ReadFile( path, &data ) ){ std::fprintf( stderr, "%s: 開けません\n", path.c_str() ); return 1; }
	std::vector<uint8_t> bytes( data.begin(), data.end() );
	std::string text, err;
	if( !Disassemble( bytes, &text, &err ) ){ std::fprintf( stderr, "%s: %s\n", path.c_str(), err.c_str() ); return 1; }
	std::fputs( text.c_str(), stdout );
	return 0;
}

}	// namespace

int main( int argc, char** argv )
{
#if defined(_WIN32)
	SetConsoleOutputCP( CP_UTF8 );
#endif
	if( argc < 2 ){ std::fputs( kUsage, stderr ); return 2; }
	std::string cmd = argv[1];
	if( cmd == "--version" ){ std::printf( "atsc %d.%d (atsb format 1.0)\n", ATS_API_VERSION_MAJOR, ATS_API_VERSION_MINOR ); return 0; }
	if( cmd == "--help" || cmd == "-h" ){ std::fputs( kUsage, stdout ); return 0; }
	if( cmd == "lsp" ) return RunLanguageServer();		// 言語サーバー（標準入出力。VS Code 拡張から起動する）

	std::vector<std::string> inputs;
	std::string manifestPath, output, lang, nameSpace;
	bool json = false, debugInfo = true;
	for( int i = 2; i < argc; ++i ){
		std::string a = argv[i];
		if( (a == "-m" || a == "--manifest") && i + 1 < argc )	manifestPath = argv[++i];
		else if( (a == "-o" || a == "--output") && i + 1 < argc )	output = argv[++i];
		else if( a == "--lang" && i + 1 < argc )					lang = argv[++i];
		else if( a == "--namespace" && i + 1 < argc )				nameSpace = argv[++i];
		else if( a == "--json" )									json = true;
		else if( a == "--no-debug" )								debugInfo = false;
		else if( !a.empty() && a[0] == '-' ){ std::fprintf( stderr, "未知のオプション %s\n\n%s", a.c_str(), kUsage ); return 2; }
		else inputs.push_back( a );
	}

	if( cmd == "disasm" ){
		if( inputs.size() != 1 ){ std::fputs( kUsage, stderr ); return 2; }
		return Disasm( inputs[0] );
	}
	if( cmd == "gen" ){
		if( manifestPath.empty() || !inputs.empty() || (lang != "cpp" && lang != "html") ){ std::fputs( kUsage, stderr ); return 2; }
		Diagnostics diag;
		Manifest manifest;
		if( !manifest.Load( manifestPath, diag ) ){ Print( diag, json ); return 1; }
		GenOptions opt;
		opt.nameSpace = nameSpace;
		opt.source    = FileName( manifestPath );
		std::string text = lang == "cpp" ? GenerateCpp( manifest, opt ) : GenerateHtml( manifest, opt );
		if( output.empty() ){
			std::fputs( text.c_str(), stdout );
		} else if( !WriteFile( output, std::vector<uint8_t>( text.begin(), text.end() ) ) ){
			std::fprintf( stderr, "%s: 書き込めません\n", output.c_str() );
			return 1;
		}
		if( !diag.List().empty() ) Print( diag, json );	// 警告があれば出す
		return 0;
	}
	if( cmd != "compile" && cmd != "validate" ){ std::fputs( kUsage, stderr ); return 2; }
	if( inputs.empty() || manifestPath.empty() ){ std::fputs( kUsage, stderr ); return 2; }
	if( !output.empty() && EndsWith( output, ".atsb" ) && inputs.size() > 1 ){
		std::fputs( "複数のファイルを 1 つの .atsb には出力できません（-o にはディレクトリを指定してください）\n", stderr );
		return 2;
	}

	Diagnostics diag;
	Manifest manifest;
	if( !manifest.Load( manifestPath, diag ) ){ Print( diag, json ); return 1; }

	CompileOptions opt;
	opt.debugInfo = debugInfo;
	std::map<std::string, std::string> ids;		// スクリプトID → ファイル
	int failed = 0;

	for( const std::string& in : inputs ){
		std::string src;
		if( !ReadFile( in, &src ) ){ diag.Error( in, 0, 0, "開けません" ); ++failed; continue; }
		CompileResult res;
		if( !CompileSource( src, in, manifest, diag, &res, opt ) ){ ++failed; continue; }

		auto it = ids.find( res.scriptId );
		if( it != ids.end() ){
			diag.Error( in, 1, 1, "スクリプトID \"" + res.scriptId + "\" は " + it->second + " と重複しています" );
			++failed;
			continue;
		}
		ids[res.scriptId] = in;

		if( cmd == "compile" ){
			std::string outPath;
			if( output.empty() ) outPath = ReplaceExt( in, ".atsb" );
			else if( EndsWith( output, ".atsb" ) ) outPath = output;
			else {
				outPath = output;
				if( outPath.back() != '/' && outPath.back() != '\\' ) outPath += "/";
				outPath += ReplaceExt( FileName( in ), ".atsb" );
			}
			if( !WriteFile( outPath, res.bytes ) ){ diag.Error( outPath, 0, 0, "書き込めません" ); ++failed; }
		}
	}

	Print( diag, json );
	return failed ? 1 : 0;
}
