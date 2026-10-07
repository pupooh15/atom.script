//=========================================================================
//	AtsCompiler：atsc を子プロセスで呼んで .ats をコンパイルする
//	（コンパイラの実装は CLI・VS Code と同じものを使う。atsc が落ちてもエディタは落ちない）
//=========================================================================
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using UnityEngine;

namespace AtomScript.Editor
{
	static class AtsCompiler
	{
		[Serializable]
		public sealed class Diagnostic
		{
			public string	file;
			public int		line;
			public int		col;
			public string	severity;		// "error" / "warning"
			public string	message;

			public bool IsError => severity != "warning";
		}

		[Serializable]
		sealed class DiagnosticList { public Diagnostic[] items; }

		public sealed class Result
		{
			public bool						Success;
			public byte[]					Bytecode;
			public readonly List<Diagnostic>	Diagnostics = new List<Diagnostic>();
			public string					ToolError;		// atsc を起動できない・出力が読めないなど
		}

		const int kTimeoutMs = 30000;

		//---------------------------------------------------------------------
		// atsc の場所（設定が空ならパッケージ同梱のもの）
		//---------------------------------------------------------------------
		public static string BundledCompilerPath
		{
			get {
				var pkg = UnityEditor.PackageManager.PackageInfo.FindForAssembly( typeof( AtsCompiler ).Assembly );
				string root = pkg != null ? pkg.resolvedPath : Path.GetFullPath( "Packages/com.pupooh15.atomscript" );
				switch( Application.platform ){
				case RuntimePlatform.WindowsEditor:	return Path.Combine( root, "Editor", "Tools~", "win-x64", "atsc.exe" );
				case RuntimePlatform.OSXEditor:		return Path.Combine( root, "Editor", "Tools~", "osx", "atsc" );
				default:							return Path.Combine( root, "Editor", "Tools~", "linux-x64", "atsc" );
				}
			}
		}

		public static bool TryFindCompiler( out string path, out string error )
		{
			string set = AtomScriptSettings.instance.CompilerPath;
			path = string.IsNullOrEmpty( set ) ? BundledCompilerPath : ProjectPaths.ToFull( set );
			error = null;
			if( File.Exists( path ) ) return true;
			error = string.IsNullOrEmpty( set )
				? $"パッケージに atsc がありません: {path}\nリポジトリで CMake のビルドをするとコピーされます（または Project Settings → AtomScript でパスを指定）"
				: $"atsc が見つかりません: {path}";
			return false;
		}

		//---------------------------------------------------------------------
		// コンパイル
		//---------------------------------------------------------------------
		public static Result Compile( string sourcePath, string manifestPath )
		{
			var result = new Result();
			if( !TryFindCompiler( out string exe, out string error ) ){ result.ToolError = error; return result; }

			string outDir = Path.Combine( ProjectPaths.Root, "Temp", "AtomScript" );
			Directory.CreateDirectory( outDir );
			string outPath = Path.Combine( outDir, Guid.NewGuid().ToString( "N" ) + ".atsb" );
			try {
				var psi = new ProcessStartInfo {
					FileName				= exe,
					Arguments				= $"compile {Quote( sourcePath )} -m {Quote( manifestPath )} -o {Quote( outPath )} --json",
					UseShellExecute			= false,
					CreateNoWindow			= true,
					RedirectStandardOutput	= true,
					RedirectStandardError	= true,
					StandardOutputEncoding	= Encoding.UTF8,
					StandardErrorEncoding	= Encoding.UTF8,
					WorkingDirectory		= ProjectPaths.Root,
				};
				string stdout, stderr;
				int exit;
				using( Process p = Process.Start( psi ) ){
					var errTask = p.StandardError.ReadToEndAsync();
					stdout = p.StandardOutput.ReadToEnd();
					if( !p.WaitForExit( kTimeoutMs ) ){
						try { p.Kill(); } catch {}
						result.ToolError = $"atsc が {kTimeoutMs / 1000} 秒以内に終わりませんでした";
						return result;
					}
					stderr = errTask.Result;
					exit = p.ExitCode;
				}

				string json = stdout.Trim();
				if( json.StartsWith( "[" ) ){
					var list = JsonUtility.FromJson<DiagnosticList>( "{\"items\":" + json + "}" );
					if( list?.items != null ) result.Diagnostics.AddRange( list.items );
				}
				if( exit == 0 && File.Exists( outPath ) ){
					result.Bytecode = File.ReadAllBytes( outPath );
					result.Success = true;
				} else if( result.Diagnostics.Count == 0 ){
					string msg = (stderr + "\n" + stdout).Trim();
					result.ToolError = $"atsc が失敗しました（終了コード {exit}）" + (msg.Length > 0 ? ": " + msg : "");
				}
			} catch( Exception e ){
				result.ToolError = $"atsc を実行できません: {e.Message}";
			} finally {
				try { if( File.Exists( outPath ) ) File.Delete( outPath ); } catch {}
			}
			return result;
		}

		static string Quote( string s ) => "\"" + s.Replace( "\"", "\\\"" ) + "\"";
	}

	//=========================================================================
	//	.atsb のヘッダーからスクリプト ID とマニフェストのハッシュを読む
	//	（形式は include/atomscript/ats_format.h、docs/bytecode.md）
	//=========================================================================
	static class AtsbHeader
	{
		const uint kMagic = 0x42535441;		// 'A' 'T' 'S' 'B'
		const uint kStrs  = 0x53525453;		// 'S' 'T' 'R' 'S'
		const int  kHeaderSize = 32;
		const int  kSectionSize = 16;

		public static bool TryRead( byte[] b, out string scriptId, out ulong manifestHash )
		{
			scriptId = null;
			manifestHash = 0;
			if( b == null || b.Length < kHeaderSize || BitConverter.ToUInt32( b, 0 ) != kMagic ) return false;
			manifestHash = BitConverter.ToUInt64( b, 16 );
			uint id = BitConverter.ToUInt32( b, 24 );
			uint sections = BitConverter.ToUInt32( b, 28 );
			for( uint i = 0; i < sections; ++i ){
				int e = kHeaderSize + (int)i * kSectionSize;
				if( e + kSectionSize > b.Length ) return false;
				if( BitConverter.ToUInt32( b, e ) != kStrs ) continue;
				int off = (int)BitConverter.ToUInt32( b, e + 4 );
				int size = (int)BitConverter.ToUInt32( b, e + 8 );
				if( off < 0 || off + size > b.Length || size < 4 ) return false;
				uint count = BitConverter.ToUInt32( b, off );
				if( id >= count || 4 + (id + 1) * 4 > size ) return false;
				int s = off + (int)BitConverter.ToUInt32( b, off + 4 + (int)id * 4 );
				int end = s;
				while( end < off + size && b[end] != 0 ) ++end;
				scriptId = Encoding.UTF8.GetString( b, s, end - s );
				return true;
			}
			return false;
		}
	}
}
