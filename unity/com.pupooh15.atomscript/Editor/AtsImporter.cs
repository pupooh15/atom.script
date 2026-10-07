//=========================================================================
//	AtsImporter：.ats を保存すると atsc でコンパイルし、AtsScriptAsset にする
//	・マニフェストは Project Settings → AtomScript（未設定なら Assets に 1 つだけあるもの）
//	・エラーはコンソールに「パス(行,列): error: …」で出し、アセットは作らない
//=========================================================================
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.AssetImporters;
using UnityEngine;

namespace AtomScript.Editor
{
	[ScriptedImporter( 1, "ats" )]
	public sealed class AtsImporter : ScriptedImporter
	{
		public override void OnImportAsset( AssetImportContext ctx )
		{
			// マニフェスト・atsc が変わったら再インポートされるようにする
			ctx.DependsOnCustomDependency( ManifestDependency.Name );

			if( !ManifestLocator.TryResolve( out string manifest, out string error ) ){
				ctx.LogImportError( $"{ctx.assetPath}: {error}" );
				return;
			}

			AtsCompiler.Result r = AtsCompiler.Compile( Path.GetFullPath( ctx.assetPath ), manifest );
			foreach( AtsCompiler.Diagnostic d in r.Diagnostics ){
				string file = string.IsNullOrEmpty( d.file ) ? ctx.assetPath : ProjectPaths.ToProjectRelative( d.file );
				string msg = $"{file}({d.line},{d.col}): {(d.IsError ? "error" : "warning")}: {d.message}";
				if( d.IsError ) ctx.LogImportError( msg );
				else ctx.LogImportWarning( msg );
			}
			if( r.ToolError != null ) ctx.LogImportError( $"{ctx.assetPath}: {r.ToolError}" );
			if( !r.Success ) return;

			if( !AtsbHeader.TryRead( r.Bytecode, out string scriptId, out ulong hash ) ){
				ctx.LogImportError( $"{ctx.assetPath}: atsc の出力（.atsb）を読めません" );
				return;
			}
			AtsScriptAsset asset = AtsScriptAsset.Create( r.Bytecode, scriptId, hash );
			asset.name = Path.GetFileNameWithoutExtension( ctx.assetPath );
			ctx.AddObjectToAsset( "main", asset );
			ctx.SetMainObject( asset );
		}

		// すべての .ats を再インポートする（Project Settings のボタン）
		public static void ReimportAll()
		{
			ManifestLocator.Invalidate();
			ManifestDependency.Update( refresh: false );
			var paths = new List<string>();
			foreach( string p in AssetDatabase.GetAllAssetPaths() )
				if( p.EndsWith( ".ats", System.StringComparison.OrdinalIgnoreCase ) ) paths.Add( p );
			AssetDatabase.StartAssetEditing();
			try {
				foreach( string p in paths ) AssetDatabase.ImportAsset( p, ImportAssetOptions.ForceUpdate );
			} finally {
				AssetDatabase.StopAssetEditing();
			}
		}
	}
}
