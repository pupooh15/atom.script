//=========================================================================
//	AtsImporter のテスト
//	Assets の下に一時フォルダーを作り、マニフェストと .ats を置いてインポートする。
//	Unity はインポートエラーを後の Refresh でも出し直すので、LogAssert で数えず、ログを集めて中身を確かめる。
//=========================================================================
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using AtomScript.Editor;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using UnityEngine.TestTools;

namespace AtomScript.Tests
{
	public class ImporterTests
	{
		const string kDir = "Assets/__AtomScriptImporterTest";
		const string kManifest = kDir + "/test.atsmanifest.yaml";

		string			m_savedManifestPath;
		bool			m_savedGenerate;
		List<string>	m_errors;

		static string Src( string name ) => Path.GetFullPath( WrapperTests.DataDir + name );

		void OnLog( string message, string stack, LogType type )
		{
			if( type == LogType.Error || type == LogType.Exception ) m_errors.Add( message );
		}

		[SetUp]
		public void SetUp()
		{
			m_errors = new List<string>();
			Application.logMessageReceived += OnLog;
			if( AssetDatabase.IsValidFolder( kDir ) ) AssetDatabase.DeleteAsset( kDir );
			Directory.CreateDirectory( kDir );
			File.Copy( Src( "wrapper_test.atsmanifest.yaml" ), kManifest );
			m_savedManifestPath = AtomScriptSettings.instance.ManifestPath;
			m_savedGenerate = AtomScriptSettings.instance.GenerateCSharp;
			AtomScriptSettings.instance.GenerateCSharp = false;		// Assets に .cs を書かせない（テスト中の再コンパイルを避ける）
			AtomScriptSettings.instance.ManifestPath = kManifest;
			AssetDatabase.Refresh( ImportAssetOptions.ForceSynchronousImport );
		}

		[TearDown]
		public void TearDown()
		{
			// 先にフォルダーを消してから設定を戻す（逆だと消えたマニフェストで再インポートが走る）
			AssetDatabase.DeleteAsset( kDir );
			AtomScriptSettings.instance.ManifestPath = m_savedManifestPath;
			CSharpGenerator.RunScheduled();
			AtomScriptSettings.instance.GenerateCSharp = m_savedGenerate;
			ManifestDependency.Update( refresh: false );
			AssetDatabase.Refresh( ImportAssetOptions.ForceSynchronousImport );
			Application.logMessageReceived -= OnLog;
			LogAssert.ignoreFailingMessages = false;
		}

		void AssertError( string pattern )
		{
			Assert.IsTrue( m_errors.Exists( e => Regex.IsMatch( e, pattern ) ),
						   $"エラーが出ていない: {pattern}\n出たエラー:\n{string.Join( "\n", m_errors )}" );
		}

		static AtsScriptAsset Import( string name, string text )
		{
			string path = kDir + "/" + name;
			File.WriteAllText( path, text );
			AssetDatabase.ImportAsset( path, ImportAssetOptions.ForceUpdate | ImportAssetOptions.ForceSynchronousImport );
			return AssetDatabase.LoadAssetAtPath<AtsScriptAsset>( path );
		}

		[Test]
		public void ImportsAtsAsScriptAsset()
		{
			AtsScriptAsset asset = Import( "wrapper.ats", File.ReadAllText( Src( "wrapper_test.ats" ) ) );
			Assert.IsNotNull( asset );
			CollectionAssert.IsEmpty( m_errors );
			Assert.AreEqual( "test/wrapper", asset.ScriptId );
			Assert.AreEqual( WrapperTest.Registration.ManifestHash, asset.ManifestHash );		// 生成コードと同じマニフェスト
			CollectionAssert.AreEqual( WrapperTests.LoadBytes( "WrapperTest.atsb.bytes" ), asset.Bytecode );

			// そのままランタイムで読み込める
			using( var rt = new ScriptRuntime() ){
				rt.Log = ( l, m ) => {};
				WrapperTest.Registration.Register( rt, new NullCommands() );
				using( ScriptProgram p = rt.LoadProgram( asset ) ) Assert.AreEqual( "test/wrapper", p.ScriptId );
			}
		}

		[Test]
		public void CompileErrorIsReportedAndNoAssetIsMade()
		{
			LogAssert.ignoreFailingMessages = true;		// エラーが出るのが正しい（中身は AssertError で確かめる）
			AtsScriptAsset asset = Import( "bad.ats", "script \"test/bad\"\n\nevent Start() { Nope() }\n" );
			Assert.IsNull( asset );
			AssertError( @"^Assets/__AtomScriptImporterTest/bad\.ats\(3,\d+\): error: .*Nope" );
		}

		[Test]
		public void MissingManifestIsReported()
		{
			LogAssert.ignoreFailingMessages = true;
			AtomScriptSettings.instance.ManifestPath = kDir + "/none.atsmanifest.yaml";
			Assert.IsNull( Import( "nomani.ats", "script \"test/nomani\"\n" ) );
			AssertError( @"nomani\.ats: マニフェストが見つかりません" );
		}

		[Test]
		public void ManifestChangeReimportsScripts()
		{
			AtsScriptAsset before = Import( "wrapper.ats", File.ReadAllText( Src( "wrapper_test.ats" ) ) );
			Assert.IsNotNull( before );
			ulong oldHash = before.ManifestHash;

			// マニフェストを変える → カスタム依存のハッシュが変わり、.ats が再インポートされる
			File.AppendAllText( kManifest, "\n# changed\n" );
			ManifestDependency.Update( refresh: false );
			AssetDatabase.Refresh( ImportAssetOptions.ForceSynchronousImport );

			AtsScriptAsset after = AssetDatabase.LoadAssetAtPath<AtsScriptAsset>( kDir + "/wrapper.ats" );
			Assert.IsNotNull( after );
			Assert.AreNotEqual( oldHash, after.ManifestHash );
		}

		[Test]
		public void HeaderReaderReadsScriptId()
		{
			Assert.IsTrue( AtsbHeader.TryRead( WrapperTests.LoadBytes( "SampleRpg.atsb.bytes" ), out string id, out ulong hash ) );
			Assert.AreEqual( "sample/merchant", id );
			Assert.AreEqual( SampleRpg.Registration.ManifestHash, hash );
			Assert.IsFalse( AtsbHeader.TryRead( new byte[] { 1, 2, 3 }, out _, out _ ) );
		}

		// 読み込みの確認用（呼ばれない）
		sealed class NullCommands : WrapperTest.IWrapperTestCommands
		{
			static Awaitable Done() { var s = new AwaitableCompletionSource(); s.SetResult(); return s.Awaitable; }
			public void Log( string text ) {}
			public Awaitable Wait( string key, System.Threading.CancellationToken ct ) => Done();
			public Awaitable<int> Ask( WrapperTest.Mood mood, System.Threading.CancellationToken ct ) { var s = new AwaitableCompletionSource<int>(); s.SetResult( 0 ); return s.Awaitable; }
			public Awaitable<string> AskName( System.Threading.CancellationToken ct ) { var s = new AwaitableCompletionSource<string>(); s.SetResult( "" ); return s.Awaitable; }
			public void Boom() {}
			public Awaitable Speak( string text, System.Threading.CancellationToken ct ) => Done();
			public int Twice( int x ) => x;
			public string NameOf( AtsHandle who ) => "";
		}
	}
}
