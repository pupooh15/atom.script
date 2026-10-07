//=========================================================================
//	AtomScriptSettings：プロジェクトの設定（ProjectSettings/AtomScriptSettings.asset）
//	Project Settings → AtomScript で編集する。
//=========================================================================
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace AtomScript.Editor
{
	[FilePath( "ProjectSettings/AtomScriptSettings.asset", FilePathAttribute.Location.ProjectFolder )]
	public sealed class AtomScriptSettings : ScriptableSingleton<AtomScriptSettings>
	{
		[SerializeField] string m_manifestPath = string.Empty;		// プロジェクトからの相対パス。空なら Assets から自動で探す
		[SerializeField] string m_compilerPath = string.Empty;		// 空ならパッケージ同梱の atsc
		[SerializeField] bool   m_generateCSharp = true;			// マニフェストが変わったら C# の登録コードを作り直す
		[SerializeField] string m_csharpOutputPath = DefaultCSharpOutputPath;	// {Project} はマニフェストの project（PascalCase）
		[SerializeField] string m_csharpNamespace = string.Empty;	// 空なら project（PascalCase）

		public const string DefaultCSharpOutputPath = "Assets/AtomScript/Generated/{Project}.g.cs";

		public string ManifestPath
		{
			get => m_manifestPath;
			set { if( m_manifestPath == value ) return; m_manifestPath = value ?? string.Empty; Changed(); }
		}

		public string CompilerPath
		{
			get => m_compilerPath;
			set { if( m_compilerPath == value ) return; m_compilerPath = value ?? string.Empty; Changed(); }
		}

		public bool GenerateCSharp
		{
			get => m_generateCSharp;
			set { if( m_generateCSharp == value ) return; m_generateCSharp = value; Save( true ); if( value ) CSharpGenerator.Schedule(); }
		}

		public string CSharpOutputPath
		{
			get => string.IsNullOrEmpty( m_csharpOutputPath ) ? DefaultCSharpOutputPath : m_csharpOutputPath;
			set { if( m_csharpOutputPath == value ) return; m_csharpOutputPath = value ?? string.Empty; Save( true ); CSharpGenerator.Schedule(); }
		}

		public string CSharpNamespace
		{
			get => m_csharpNamespace;
			set { if( m_csharpNamespace == value ) return; m_csharpNamespace = value ?? string.Empty; Save( true ); CSharpGenerator.Schedule(); }
		}

		void Changed()
		{
			Save( true );
			ManifestDependency.Update( refresh: true );
		}
	}

	//=========================================================================
	//	Project Settings の画面
	//=========================================================================
	sealed class AtomScriptSettingsProvider : SettingsProvider
	{
		AtomScriptSettingsProvider() : base( "Project/AtomScript", SettingsScope.Project, new[] { "AtomScript", "ats", "manifest" } ) {}

		[SettingsProvider]
		static SettingsProvider Create() => new AtomScriptSettingsProvider();

		public override void OnGUI( string searchContext )
		{
			AtomScriptSettings s = AtomScriptSettings.instance;
			EditorGUIUtility.labelWidth = 160;

			EditorGUILayout.Space();
			EditorGUILayout.LabelField( "マニフェスト", EditorStyles.boldLabel );
			using( new EditorGUILayout.HorizontalScope() ){
				string p = EditorGUILayout.DelayedTextField( new GUIContent( "パス", "プロジェクトからの相対パス。空なら Assets にある *.atsmanifest.yaml が 1 つだけのときそれを使う" ), s.ManifestPath );
				if( GUILayout.Button( "選択…", GUILayout.Width( 60 ) ) ){
					string picked = EditorUtility.OpenFilePanel( "マニフェストを選ぶ", ProjectPaths.Root, "yaml" );
					if( !string.IsNullOrEmpty( picked ) ) p = ProjectPaths.ToProjectRelative( picked );
				}
				s.ManifestPath = p;
			}
			if( ManifestLocator.TryResolve( out string manifest, out string error ) )
				EditorGUILayout.HelpBox( "使用中：" + ProjectPaths.ToProjectRelative( manifest ), MessageType.Info );
			else
				EditorGUILayout.HelpBox( error, MessageType.Error );

			EditorGUILayout.Space();
			EditorGUILayout.LabelField( "コンパイラ（atsc）", EditorStyles.boldLabel );
			s.CompilerPath = EditorGUILayout.DelayedTextField( new GUIContent( "パス", "空ならパッケージに同梱の atsc を使う" ), s.CompilerPath );
			if( AtsCompiler.TryFindCompiler( out string exe, out error ) )
				EditorGUILayout.HelpBox( "使用中：" + exe, MessageType.Info );
			else
				EditorGUILayout.HelpBox( error, MessageType.Error );

			EditorGUILayout.Space();
			EditorGUILayout.LabelField( "C# の登録コード（atsc gen）", EditorStyles.boldLabel );
			s.GenerateCSharp = EditorGUILayout.Toggle( new GUIContent( "自動で生成", "マニフェストが変わったら作り直す（中身が同じなら書き換えない）" ), s.GenerateCSharp );
			s.CSharpOutputPath = EditorGUILayout.DelayedTextField( new GUIContent( "出力先", "プロジェクトからの相対パス。{Project} はマニフェストの project（PascalCase）" ), s.CSharpOutputPath );
			s.CSharpNamespace = EditorGUILayout.DelayedTextField( new GUIContent( "名前空間", "空ならマニフェストの project（PascalCase）" ), s.CSharpNamespace );
			if( ManifestLocator.TryResolve( out manifest, out _ ) )
				EditorGUILayout.LabelField( " ", "→ " + CSharpGenerator.OutputPathFor( manifest ) );
			if( GUILayout.Button( "今すぐ生成", GUILayout.Width( 220 ) ) ) CSharpGenerator.GenerateNow( log: true );

			EditorGUILayout.Space();
			if( GUILayout.Button( "すべての .ats を再インポート", GUILayout.Width( 220 ) ) ) AtsImporter.ReimportAll();
		}
	}

	//=========================================================================
	//	パスの変換
	//=========================================================================
	static class ProjectPaths
	{
		public static string Root => Path.GetFullPath( Path.Combine( Application.dataPath, ".." ) );

		public static string ToProjectRelative( string path )
		{
			string full = Path.GetFullPath( path ).Replace( '\\', '/' );
			string root = Root.Replace( '\\', '/' ).TrimEnd( '/' ) + "/";
			return full.StartsWith( root, System.StringComparison.OrdinalIgnoreCase ) ? full.Substring( root.Length ) : full;
		}

		public static string ToFull( string projectRelative ) => Path.GetFullPath( Path.Combine( Root, projectRelative ) );
	}

	//=========================================================================
	//	使うマニフェストを決める
	//=========================================================================
	static class ManifestLocator
	{
		const string kPattern = "*.atsmanifest.yaml";
		static string[] s_found;		// Assets から探した結果（マニフェストが増減したら捨てる）

		public static void Invalidate() => s_found = null;

		public static bool TryResolve( out string fullPath, out string error )
		{
			fullPath = null;
			error = null;
			string set = AtomScriptSettings.instance.ManifestPath;
			if( !string.IsNullOrEmpty( set ) ){
				fullPath = ProjectPaths.ToFull( set );
				if( File.Exists( fullPath ) ) return true;
				error = $"マニフェストが見つかりません: {set}（Project Settings → AtomScript）";
				fullPath = null;
				return false;
			}
			if( s_found == null ){
				var list = new List<string>();
				try {
					list.AddRange( Directory.GetFiles( Application.dataPath, kPattern, SearchOption.AllDirectories ) );
				} catch( IOException ){}
				list.Sort( System.StringComparer.Ordinal );
				s_found = list.ToArray();
			}
			if( s_found.Length == 1 ){ fullPath = s_found[0]; return true; }
			error = s_found.Length == 0
				? "マニフェスト（*.atsmanifest.yaml）が Assets にありません。Project Settings → AtomScript でパスを指定してください"
				: "マニフェストが複数あります。Project Settings → AtomScript でどれを使うか指定してください:\n  " +
				  string.Join( "\n  ", System.Array.ConvertAll( s_found, ProjectPaths.ToProjectRelative ) );
			return false;
		}
	}
}
