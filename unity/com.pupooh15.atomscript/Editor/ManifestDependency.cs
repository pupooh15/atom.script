//=========================================================================
//	ManifestDependency：マニフェスト（include 先を含む）と atsc が変わったら .ats を再インポートさせる
//	Unity のカスタム依存（AssetDatabase.RegisterCustomDependency）に内容のハッシュを登録し、
//	各 .ats のインポートがそれに依存する。ハッシュが変わると、次の Refresh で .ats がすべて再インポートされる。
//=========================================================================
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using UnityEditor;
using UnityEngine;

namespace AtomScript.Editor
{
	[InitializeOnLoad]
	static class ManifestDependency
	{
		public const string Name = "AtomScript/Manifest";

		static Hash128 s_registered;
		static bool s_refreshQueued;

		static ManifestDependency()
		{
			// エディタの外でマニフェストを書き換えた・atsc を作り直した場合は、エディタに戻ったときに拾う
			EditorApplication.focusChanged += focused => { if( focused ) Update( refresh: true ); };
			Update( refresh: false );
		}

		// 現在のハッシュを登録する。変わっていて refresh なら、.ats を再インポートさせる
		public static void Update( bool refresh )
		{
			Hash128 h = Compute();
			if( h == s_registered ) return;
			s_registered = h;
			AssetDatabase.RegisterCustomDependency( Name, h );
			if( refresh && !s_refreshQueued ){
				s_refreshQueued = true;
				EditorApplication.delayCall += () => { s_refreshQueued = false; AssetDatabase.Refresh(); };
			}
		}

		public static Hash128 Compute()
		{
			var sb = new StringBuilder();
			if( ManifestLocator.TryResolve( out string manifest, out string error ) ){
				AppendFile( sb, manifest, new HashSet<string>( StringComparer.OrdinalIgnoreCase ), 0 );
			} else {
				sb.Append( "error:" ).Append( error );
			}
			if( AtsCompiler.TryFindCompiler( out string exe, out _ ) ){
				var fi = new FileInfo( exe );
				sb.Append( "\natsc:" ).Append( exe ).Append( ':' ).Append( fi.Length ).Append( ':' ).Append( fi.LastWriteTimeUtc.Ticks );
			} else {
				sb.Append( "\natsc:none" );
			}
			return Hash128.Compute( sb.ToString() );
		}

		// ファイルの内容と、include したファイルの内容をつなげる
		static void AppendFile( StringBuilder sb, string path, HashSet<string> seen, int depth )
		{
			string full = Path.GetFullPath( path );
			if( depth > 16 || !seen.Add( full ) ) return;
			sb.Append( "\nfile:" ).Append( full ).Append( '\n' );
			string text;
			try { text = File.ReadAllText( full ); } catch( IOException ){ sb.Append( "<missing>" ); return; }
			sb.Append( text );
			string dir = Path.GetDirectoryName( full );
			foreach( string inc in Includes( text ) ) AppendFile( sb, Path.Combine( dir, inc ), seen, depth + 1 );
		}

		// トップレベルの include: に並んだパス（"- path" の形と [a, b] の形）
		static IEnumerable<string> Includes( string yaml )
		{
			bool inInclude = false;
			foreach( string raw in yaml.Split( '\n' ) ){
				string line = StripComment( raw ).TrimEnd();
				if( line.Length == 0 ) continue;
				if( !char.IsWhiteSpace( line[0] ) && line[0] != '-' ){
					inInclude = false;
					if( line.StartsWith( "include:" ) ){
						string rest = line.Substring( 8 ).Trim();
						if( rest.StartsWith( "[" ) && rest.EndsWith( "]" ) ){
							foreach( string p in rest.Substring( 1, rest.Length - 2 ).Split( ',' ) ){
								string v = Unquote( p.Trim() );
								if( v.Length > 0 ) yield return v;
							}
						} else {
							inInclude = rest.Length == 0;
						}
					}
					continue;
				}
				string t = line.Trim();
				if( inInclude && t.StartsWith( "-" ) ){
					string v = Unquote( t.Substring( 1 ).Trim() );
					if( v.Length > 0 ) yield return v;
				}
			}
		}

		static string StripComment( string s )
		{
			bool q1 = false, q2 = false;
			for( int i = 0; i < s.Length; ++i ){
				char c = s[i];
				if( c == '\'' && !q2 ) q1 = !q1;
				else if( c == '"' && !q1 ) q2 = !q2;
				else if( c == '#' && !q1 && !q2 && (i == 0 || char.IsWhiteSpace( s[i - 1] )) ) return s.Substring( 0, i );
			}
			return s.TrimEnd( '\r' );
		}

		static string Unquote( string s )
		{
			if( s.Length >= 2 && ((s[0] == '"' && s[s.Length - 1] == '"') || (s[0] == '\'' && s[s.Length - 1] == '\'')) ) return s.Substring( 1, s.Length - 2 );
			return s;
		}
	}

	//=========================================================================
	//	Assets 内のマニフェストが増減・変更されたとき
	//=========================================================================
	sealed class ManifestPostprocessor : AssetPostprocessor
	{
		static void OnPostprocessAllAssets( string[] imported, string[] deleted, string[] moved, string[] movedFrom )
		{
			if( !Touches( imported ) && !Touches( deleted ) && !Touches( moved ) && !Touches( movedFrom ) ) return;
			ManifestLocator.Invalidate();
			ManifestDependency.Update( refresh: true );
		}

		static bool Touches( string[] paths )
		{
			foreach( string p in paths ) if( p.EndsWith( ".yaml", StringComparison.OrdinalIgnoreCase ) || p.EndsWith( ".yml", StringComparison.OrdinalIgnoreCase ) ) return true;
			return false;
		}
	}
}
