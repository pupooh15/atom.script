//=========================================================================
//	仕様書 §4 の例（samples/merchant.ats）を、生成コード SampleRpg.g.cs 経由で動かす
//=========================================================================
using System.Collections.Generic;
using System.Threading;
using NUnit.Framework;
using SampleRpg;
using UnityEngine;

namespace AtomScript.Tests
{
	public class SampleRpgTests
	{
		// すべての待機ありコマンドをその場で終わらせる実装
		class Commands : ISampleRpgCommands
		{
			public readonly List<string>	Log = new List<string>();
			public readonly Queue<int>		Answers = new Queue<int>();
			public bool						QuestCleared;

			static Awaitable Done()
			{
				var s = new AwaitableCompletionSource();
				s.SetResult();
				return s.Awaitable;
			}

			public Awaitable ShowMessage( string text, Face face, CancellationToken ct ) { Log.Add( $"msg:{text}:{face}" ); return Done(); }

			public Awaitable<int> SelectWindow( SelectType type, CancellationToken ct )
			{
				Log.Add( $"select:{type}" );
				var s = new AwaitableCompletionSource<int>();
				s.SetResult( Answers.Count > 0 ? Answers.Dequeue() : 0 );
				return s.Awaitable;
			}

			public void CameraShake( float power, float duration ) => Log.Add( $"shake:{power}:{duration}" );
			public Awaitable FadeOut( float time, CancellationToken ct ) { Log.Add( $"fade:{time}" ); return Done(); }
			public Awaitable PlayBgm( string name, CancellationToken ct ) { Log.Add( $"bgm:{name}" ); return Done(); }
			public bool IsQuestCleared( int questId ) { Log.Add( $"cleared?{questId}" ); return QuestCleared; }
		}

		[Test]
		public void MerchantRunsToEnd()
		{
			var cmd = new Commands();
			cmd.Answers.Enqueue( 1 );		// 「本当に？」を 1 回挟む
			cmd.Answers.Enqueue( 0 );
			using( var rt = new ScriptRuntime() ){
				var errors = new List<string>();
				rt.Log = ( l, m ) => { if( l == AtsLogLevel.Error ) errors.Add( m ); };
				Registration.Register( rt, cmd );
				using( ScriptProgram prog = rt.LoadProgram( WrapperTests.LoadBytes( "SampleRpg.atsb.bytes" ) ) )
				using( ScriptVM vm = rt.CreateVM() ){
					Assert.AreEqual( "sample/merchant", prog.ScriptId );
					vm.Set( Vars.Story.Chapter, 3 );
					FiberId f = Events.FireOnTalk( vm, prog, new AtsHandle( 7 ) );
					vm.Update( 0 );
					vm.Update( 1.0f );
					vm.Update( 1.0f );		// wait 1.5
					Assert.IsFalse( vm.IsFiberAlive( f ) );
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
					CollectionAssert.IsEmpty( errors );
				}
			}
		}
	}
}
