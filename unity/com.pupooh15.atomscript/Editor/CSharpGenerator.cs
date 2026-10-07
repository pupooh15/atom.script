//=========================================================================
//	CSharpGenerator：マニフェストから C# の登録コードを作る（atsc gen --lang csharp）
//	・自動：マニフェスト（include 先を含む）と atsc が変わったら作り直す（Project Settings でオフにできる）
//	・手動：Assets → AtomScript → C# を生成、Project Settings の「今すぐ生成」
//	中身が同じなら書き換えない（無駄なスクリプトの再コンパイルを起こさない）。
//=========================================================================
using System.IO;
using System.Text;
using UnityEditor;
using UnityEngine;

namespace AtomScript.Editor
{
	public static class CSharpGenerator
	{
		public enum Outcome { Written, Unchanged, Skipped, Failed }

		// 最後に生成したときのマニフェストのハッシュ（ドメインリロードをまたいで覚える。エディタを起動し直すと 1 回は確かめる）
		const string kStateKey = "AtomScript.CSharpGenerator.Hash";
		static bool s_scheduled;

		[MenuItem( "Assets/AtomScript/C# を生成", priority = 1100 )]
		static void MenuGenerate() => GenerateNow( log: true );

		// マニフェストのハッシュが変わったとき（ManifestDependency から呼ばれる）
		internal static void OnManifestHash( Hash128 hash )
		{
			if( !AtomScriptSettings.instance.GenerateCSharp ) return;
			if( SessionState.GetString( kStateKey, "" ) == hash.ToString() ) return;
			Schedule();
		}

		// 次のエディタ更新で生成する（インポート中・初期化中はアセットを書き出せないため）
		public static void Schedule()
		{
			if( s_scheduled ) return;
			s_scheduled = true;
			EditorApplication.delayCall += RunScheduled;
		}

		// 予約済みの生成をいま行う（テスト用）
		internal static void RunScheduled()
		{
			if( !s_scheduled ) return;
			s_scheduled = false;
			if( AtomScriptSettings.instance.GenerateCSharp ) GenerateNow( log: false );
		}

		internal static bool IsScheduled => s_scheduled;

		//---------------------------------------------------------------------
		// 生成
		//---------------------------------------------------------------------
		public static Outcome GenerateNow( bool log )
		{
			if( !ManifestLocator.TryResolve( out string manifest, out string error ) ){
				// マニフェストの問題はインポーターも報告するので、自動のときは黙る
				if( log ) Debug.LogError( "[AtomScript] C# を生成できません: " + error );
				return Outcome.Skipped;
			}
			// 失敗しても同じマニフェストで何度もエラーを出さないよう、先に覚える
			SessionState.SetString( kStateKey, ManifestDependency.Compute().ToString() );
			string output = OutputPathFor( manifest );

			AtsCompiler.Result r = AtsCompiler.GenerateCSharp( manifest, AtomScriptSettings.instance.CSharpNamespace, out string text );
			foreach( AtsCompiler.Diagnostic d in r.Diagnostics ){
				string msg = $"{ProjectPaths.ToProjectRelative( string.IsNullOrEmpty( d.file ) ? manifest : d.file )}({d.line},{d.col}): {(d.IsError ? "error" : "warning")}: {d.message}";
				if( d.IsError ) Debug.LogError( msg ); else Debug.LogWarning( msg );
			}
			if( r.ToolError != null ) Debug.LogError( "[AtomScript] C# を生成できません: " + r.ToolError );
			if( !r.Success ) return Outcome.Failed;

			string full = ProjectPaths.ToFull( output );
			if( File.Exists( full ) && File.ReadAllText( full, Encoding.UTF8 ) == text ){
				if( log ) Debug.Log( $"[AtomScript] {output} は最新です" );
				return Outcome.Unchanged;
			}
			Directory.CreateDirectory( Path.GetDirectoryName( full ) );
			File.WriteAllText( full, text, new UTF8Encoding( false ) );
			if( IsInAssetDatabase( output ) ) AssetDatabase.ImportAsset( output );
			Debug.Log( $"[AtomScript] {output} を生成しました" );
			return Outcome.Written;
		}

		// 出力先（{Project} を置き換えたもの。プロジェクトからの相対パス）
		public static string OutputPathFor( string manifestPath )
		{
			string path = AtomScriptSettings.instance.CSharpOutputPath.Replace( '\\', '/' );
			return path.Replace( "{Project}", Pascal( ReadProject( manifestPath ) ) );
		}

		static bool IsInAssetDatabase( string projectRelative )
			=> projectRelative.StartsWith( "Assets/" ) || projectRelative.StartsWith( "Packages/" );

		// マニフェストのトップレベルの project:（無ければ atsc と同じく AtomScriptProject）
		static string ReadProject( string manifestPath )
		{
			try {
				foreach( string raw in File.ReadAllLines( manifestPath ) ){
					if( !raw.StartsWith( "project:" ) ) continue;
					string v = raw.Substring( 8 );
					int hash = v.IndexOf( " #" );
					if( hash >= 0 ) v = v.Substring( 0, hash );
					v = v.Trim().Trim( '"', '\'' );
					if( v.Length > 0 ) return v;
				}
			} catch( IOException ){}
			return "AtomScriptProject";
		}

		// snake_case → PascalCase（atsc の Pascal() と同じ規則）
		internal static string Pascal( string s )
		{
			var sb = new StringBuilder();
			bool up = true;
			foreach( char c in s ){
				if( c == '_' || c == '-' || c == '.' || c == ' ' ){ up = true; continue; }
				sb.Append( up && c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c );
				up = false;
			}
			return sb.Length > 0 ? sb.ToString() : s;
		}
	}
}
