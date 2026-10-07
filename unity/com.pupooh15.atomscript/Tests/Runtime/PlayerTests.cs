//=========================================================================
//	プレイヤー（PlayMode・実機）で動かすテスト
//	ネイティブプラグインの読み込み、PlayerLoop での更新、IL2CPP でのコールバック（コマンド・キャンセル・ログ）を確かめる。
//	プレイヤーではファイルを読めないので、.atsb は SampleRpgBytecode.g.cs（CMake のビルドで生成）に埋め込んである
//=========================================================================
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using NUnit.Framework;
using SampleRpg;
using UnityEngine;
using UnityEngine.TestTools;

namespace AtomScript.Tests
{
	public class PlayerTests
	{
		// 待機ありコマンドは数フレームかけて終わる（PlayerLoop の更新を通す）
		class Commands : ISampleRpgCommands
		{
			public readonly List<string>	Log = new List<string>();
			public readonly Queue<int>		Answers = new Queue<int>();
			public bool						Block;			// true なら ShowMessage はキャンセルされるまで終わらない
			public bool						Cancelled;

			static async Awaitable Frames( int n, CancellationToken ct )
			{
				for( int i = 0; i < n; ++i ) await Awaitable.NextFrameAsync( ct );
			}

			public async Awaitable ShowMessage( string text, Face face, CancellationToken ct )
			{
				Log.Add( $"msg:{text}:{face}" );
				if( Block ){
					ct.Register( () => Cancelled = true );
					while( true ) await Awaitable.NextFrameAsync( ct );
				}
				await Frames( 2, ct );
			}

			public async Awaitable<int> SelectWindow( SelectType type, CancellationToken ct )
			{
				Log.Add( $"select:{type}" );
				await Frames( 2, ct );
				return Answers.Count > 0 ? Answers.Dequeue() : 0;
			}

			public void CameraShake( float power, float duration ) => Log.Add( $"shake:{power}:{duration}" );
			public Awaitable FadeOut( float time, CancellationToken ct ) { Log.Add( $"fade:{time}" ); return Frames( 1, ct ); }
			public Awaitable PlayBgm( string name, CancellationToken ct ) { Log.Add( $"bgm:{name}" ); return Frames( 1, ct ); }
			public bool IsQuestCleared( int questId ) { Log.Add( $"cleared?{questId}" ); return false; }
		}

#if ENABLE_IL2CPP
		const string kBackend = "IL2CPP";
#else
		const string kBackend = "Mono";
#endif

		[UnityTest]
		public IEnumerator MerchantRunsOnPlayerLoop()
		{
			Debug.Log( $"AtomScript PlayerTests: {Application.platform} {RuntimeInformation.ProcessArchitecture} {kBackend}" );
			var cmd = new Commands();
			cmd.Answers.Enqueue( 1 );		// 「本当に？」を 1 回挟む
			cmd.Answers.Enqueue( 0 );
			var errors = new List<string>();
			using( var rt = new ScriptRuntime() ){
				rt.Log = ( l, m ) => { if( l == AtsLogLevel.Error ) errors.Add( m ); };
				Registration.Register( rt, cmd );
				using( ScriptProgram prog = rt.LoadProgram( SampleRpgBytecode.Bytes() ) )
				using( ScriptVM vm = rt.CreateVM() ){
					Assert.AreEqual( "sample/merchant", prog.ScriptId );
					AtomScriptLoop.Register( vm );
					try {
						vm.Set( Vars.Story.Chapter, 3 );
						FiberId f = Events.FireOnTalk( vm, prog, new AtsHandle( 7 ) );
						float limit = Time.realtimeSinceStartup + 10;		// スクリプトに wait 1.5 がある
						while( vm.IsFiberAlive( f ) && Time.realtimeSinceStartup < limit ) yield return null;
						Assert.IsFalse( vm.IsFiberAlive( f ), "ファイバが終わらない" );
					} finally {
						AtomScriptLoop.Unregister( vm );
					}
					CollectionAssert.AreEqual( new[] {
						"cleared?12",
						"msg:例の品は手に入ったかい？:Smile",
						"shake:2:0.5",
						"select:YesNo",
						"msg:本当に？:Normal",
						"select:YesNo",
						"fade:0.5",
						"bgm:bgm_shop",
					}, cmd.Log );
					Assert.IsTrue( vm.Get( Vars.Story.MetMerchant ) );
					Assert.AreEqual( 1, vm.Get( Vars.Scene.RewardCount ) );

					// セーブして別の VM にロードする
					byte[] data = vm.Save();
					using( ScriptVM vm2 = rt.CreateVM() ){
						vm2.Load( data );
						Assert.IsTrue( vm2.Get( Vars.Story.MetMerchant ) );
						Assert.AreEqual( 3, vm2.Get( Vars.Story.Chapter ) );
					}
				}
			}
			CollectionAssert.IsEmpty( errors );
		}

		[UnityTest]
		public IEnumerator AbortCancelsPendingCommand()
		{
			var cmd = new Commands { Block = true };
			using( var rt = new ScriptRuntime() ){
				Registration.Register( rt, cmd );
				using( ScriptProgram prog = rt.LoadProgram( SampleRpgBytecode.Bytes() ) )
				using( ScriptVM vm = rt.CreateVM() ){
					AtomScriptLoop.Register( vm );
					try {
						vm.Set( Vars.Story.Chapter, 3 );
						FiberId f = Events.FireOnTalk( vm, prog, new AtsHandle( 7 ) );
						for( int i = 0; i < 5; ++i ) yield return null;
						Assert.IsTrue( vm.IsFiberAlive( f ) );
						Assert.IsTrue( vm.AbortFiber( f ) );
						yield return null;
						Assert.IsTrue( cmd.Cancelled, "キャンセルが届かない" );
						Assert.IsFalse( vm.IsFiberAlive( f ) );
					} finally {
						AtomScriptLoop.Unregister( vm );
					}
				}
			}
		}
	}
}
