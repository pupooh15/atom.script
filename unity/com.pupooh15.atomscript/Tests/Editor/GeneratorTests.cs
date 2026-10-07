//=========================================================================
//	CSharpGenerator（エディタからの atsc gen）のテスト
//	出力先は Temp の下にする（Assets に .cs を書くとテスト中にスクリプトの再コンパイルが起きるため）。
//=========================================================================
using System.IO;
using AtomScript.Editor;
using NUnit.Framework;
using UnityEditor;

namespace AtomScript.Tests
{
	public class GeneratorTests
	{
		const string kDir = "Assets/__AtomScriptGenTest";
		// atsc の出力のコメントにマニフェストのファイル名が入るので、パッケージの生成物と同じ名前にする
		const string kManifest = kDir + "/wrapper_test.atsmanifest.yaml";
		const string kOutDir = "Temp/AtomScriptGenTest";
		const string kOutput = kOutDir + "/WrapperTest.g.cs";

		string	m_manifestPath, m_outputPath, m_namespace;
		bool	m_generate;

		[SetUp]
		public void SetUp()
		{
			AtomScriptSettings s = AtomScriptSettings.instance;
			m_manifestPath = s.ManifestPath;
			m_outputPath = s.CSharpOutputPath;
			m_namespace = s.CSharpNamespace;
			m_generate = s.GenerateCSharp;
			s.GenerateCSharp = false;		// 自動生成はテストの中で明示的に動かす

			if( AssetDatabase.IsValidFolder( kDir ) ) AssetDatabase.DeleteAsset( kDir );
			Directory.CreateDirectory( kDir );
			File.Copy( Path.GetFullPath( WrapperTests.DataDir + "wrapper_test.atsmanifest.yaml" ), kManifest );
			if( Directory.Exists( kOutDir ) ) Directory.Delete( kOutDir, true );
			s.ManifestPath = kManifest;
			s.CSharpOutputPath = kOutDir + "/{Project}.g.cs";
			s.CSharpNamespace = "";
			CSharpGenerator.RunScheduled();		// 設定の変更で予約された分を片付ける（自動はオフなので何もしない）
		}

		[TearDown]
		public void TearDown()
		{
			AssetDatabase.DeleteAsset( kDir );
			if( Directory.Exists( kOutDir ) ) Directory.Delete( kOutDir, true );
			AtomScriptSettings s = AtomScriptSettings.instance;
			s.GenerateCSharp = false;
			s.ManifestPath = m_manifestPath;
			s.CSharpOutputPath = m_outputPath;
			s.CSharpNamespace = m_namespace;
			CSharpGenerator.RunScheduled();
			s.GenerateCSharp = m_generate;
			ManifestDependency.Update( refresh: false );
			AssetDatabase.Refresh( ImportAssetOptions.ForceSynchronousImport );
		}

		[Test]
		public void OutputPathExpandsProject()
		{
			Assert.AreEqual( kOutput, CSharpGenerator.OutputPathFor( Path.GetFullPath( kManifest ) ) );
			Assert.AreEqual( "SampleRpg", CSharpGenerator.Pascal( "sample_rpg" ) );
			Assert.AreEqual( "Already", CSharpGenerator.Pascal( "Already" ) );
		}

		[Test]
		public void GeneratesSameCodeAsCli()
		{
			Assert.AreEqual( CSharpGenerator.Outcome.Written, CSharpGenerator.GenerateNow( log: false ) );
			string expected = File.ReadAllText( "Packages/com.pupooh15.atomscript/Tests/Editor/Generated/WrapperTest.g.cs" );
			Assert.AreEqual( expected, File.ReadAllText( kOutput ) );

			// 中身が同じなら書き換えない
			System.DateTime t = File.GetLastWriteTimeUtc( kOutput );
			Assert.AreEqual( CSharpGenerator.Outcome.Unchanged, CSharpGenerator.GenerateNow( log: false ) );
			Assert.AreEqual( t, File.GetLastWriteTimeUtc( kOutput ) );
		}

		[Test]
		public void NamespaceSetting()
		{
			AtomScriptSettings.instance.CSharpNamespace = "Game.Scripts";
			CSharpGenerator.RunScheduled();		// 自動はオフなので何もしない
			Assert.IsFalse( File.Exists( kOutput ) );
			Assert.AreEqual( CSharpGenerator.Outcome.Written, CSharpGenerator.GenerateNow( log: false ) );
			StringAssert.Contains( "namespace Game.Scripts", File.ReadAllText( kOutput ) );
		}

		[Test]
		public void RegeneratesWhenManifestChanges()
		{
			AtomScriptSettings.instance.GenerateCSharp = true;		// オンにすると生成が予約される
			Assert.IsTrue( CSharpGenerator.IsScheduled );
			CSharpGenerator.RunScheduled();
			string first = File.ReadAllText( kOutput );

			// マニフェストにクエリを足す（末尾は queries:）→ 予約され、interface に現れる
			File.AppendAllText( kManifest, "\n  - name: Extra\n    returns: int\n    params: [ { name: n, type: int } ]\n" );
			ManifestDependency.Update( refresh: false );
			Assert.IsTrue( CSharpGenerator.IsScheduled );
			CSharpGenerator.RunScheduled();
			string second = File.ReadAllText( kOutput );
			Assert.AreNotEqual( first, second );
			StringAssert.Contains( "int Extra( int n );", second );
		}
	}
}
