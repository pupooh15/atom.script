//=========================================================================
//	C# ラッパーのテスト（Generated/WrapperTest.g.cs と Data~/WrapperTest.atsb.bytes を使う）
//	どちらも CMake のビルドで atsc から作られる。
//=========================================================================
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.LowLevel;
using UnityEngine.TestTools;
using WrapperTest;

namespace AtomScript.Tests
{
	public class WrapperTests
	{
		internal const string DataDir = "Packages/com.pupooh15.atomscript/Tests/Editor/Data~/";

		internal static byte[] LoadBytes( string name ) => File.ReadAllBytes( Path.GetFullPath( DataDir + name ) );

		class Npc { public string Name; }

		// ゲーム側の実装
		class Commands : IWrapperTestCommands
		{
			public readonly List<string>									Log = new List<string>();
			public readonly Dictionary<string, AwaitableCompletionSource>	Waits = new Dictionary<string, AwaitableCompletionSource>();
			public readonly List<string>									Cancelled = new List<string>();
			public readonly List<AwaitableCompletionSource>					Speaks = new List<AwaitableCompletionSource>();
			public AwaitableCompletionSource<int>							AskSource;
			public AwaitableCompletionSource<string>						AskNameSource;
			public Mood														AskedMood = Mood.Calm;
			public bool														Immediate;		// 待機ありコマンドをその場で終わらせる
			public HandleTable<Npc>											Npcs = new HandleTable<Npc>();

			static Awaitable Done()
			{
				var s = new AwaitableCompletionSource();
				s.SetResult();
				return s.Awaitable;
			}

			void IWrapperTestCommands.Log( string text ) => Log.Add( text );

			public Awaitable Wait( string key, CancellationToken ct )
			{
				if( Immediate ) return Done();
				var s = new AwaitableCompletionSource();
				Waits[ key ] = s;
				ct.Register( () => { Cancelled.Add( key ); s.TrySetCanceled(); } );
				return s.Awaitable;
			}

			public Awaitable<int> Ask( Mood mood, CancellationToken ct )
			{
				AskedMood = mood;
				AskSource = new AwaitableCompletionSource<int>();
				if( Immediate ) AskSource.SetResult( 5 );
				return AskSource.Awaitable;
			}

			public Awaitable<string> AskName( CancellationToken ct )
			{
				AskNameSource = new AwaitableCompletionSource<string>();
				if( Immediate ) AskNameSource.SetResult( "now" );
				return AskNameSource.Awaitable;
			}

			public void Boom() => throw new InvalidOperationException( "boom" );

			public Awaitable Speak( string text, CancellationToken ct )
			{
				var s = new AwaitableCompletionSource();
				Speaks.Add( s );
				Log.Add( "speak:" + text );
				return s.Awaitable;
			}

			public int Twice( int x ) => x * 2;
			public string NameOf( AtsHandle who ) => Npcs.Get( who )?.Name ?? "?";
		}

		ScriptRuntime	m_rt;
		ScriptProgram	m_prog;
		ScriptVM		m_vm;
		Commands		m_cmd;
		List<string>	m_errors;

		[SetUp]
		public void SetUp()
		{
			m_cmd = new Commands();
			m_errors = new List<string>();
			m_rt = new ScriptRuntime();
			m_rt.Log = ( level, msg ) => { if( level == AtsLogLevel.Error ) m_errors.Add( msg ); };
			Registration.Register( m_rt, m_cmd );
			m_prog = m_rt.LoadProgram( LoadBytes( "WrapperTest.atsb.bytes" ) );
			m_vm = m_rt.CreateVM();
		}

		[TearDown]
		public void TearDown()
		{
			m_vm?.Dispose();
			m_prog?.Dispose();
			m_rt?.Dispose();
		}

		// Awaitable の続きが次のフレームに回る場合に備えて 1 フレーム進めてから update する
		IEnumerator Step( float dt = 0 )
		{
			yield return null;
			m_vm.Update( dt );
		}

		//---------------------------------------------------------------------
		[Test]
		public void ProgramHasScriptId()
		{
			Assert.AreEqual( "test/wrapper", m_prog.ScriptId );
		}

		[UnityTest]
		public IEnumerator LatentCommandsAndQueries()
		{
			FiberId f = Events.FireStart( m_vm, m_prog );
			Assert.IsTrue( f.IsValid );
			m_vm.Update( 0 );
			CollectionAssert.AreEqual( new[] { "start" }, m_cmd.Log );
			Assert.AreEqual( 42, m_vm.Get( Vars.G.Counter ) );		// クエリ Twice(21)
			Assert.IsTrue( m_cmd.Waits.ContainsKey( "a" ) );
			Assert.AreEqual( 1, m_rt.PendingCount );

			m_cmd.Waits["a"].SetResult();
			yield return Step();
			CollectionAssert.AreEqual( new[] { "start", "after a" }, m_cmd.Log );
			Assert.AreEqual( Mood.Angry, m_cmd.AskedMood );		// enum の引数

			m_cmd.AskSource.SetResult( 7 );
			yield return Step();
			Assert.AreEqual( 7, m_vm.Get( Vars.G.Counter ) );		// int の結果

			m_cmd.AskNameSource.SetResult( "アン" );
			yield return Step();
			Assert.AreEqual( "アン", m_vm.Get( Vars.G.Label ) );	// string の結果（UTF-8）
			Assert.IsTrue( m_vm.Get( Vars.G.Flag ) );
			CollectionAssert.AreEqual( new[] { "start", "after a", "done" }, m_cmd.Log );
			Assert.IsFalse( m_vm.IsFiberAlive( f ) );
			Assert.AreEqual( 0, m_rt.PendingCount );
			CollectionAssert.IsEmpty( m_errors );
		}

		[Test]
		public void AwaitableCompletedImmediately()
		{
			m_cmd.Immediate = true;
			FiberId f = Events.FireStart( m_vm, m_prog );
			m_vm.Update( 0 );
			CollectionAssert.AreEqual( new[] { "start", "after a", "done" }, m_cmd.Log );
			Assert.AreEqual( 5, m_vm.Get( Vars.G.Counter ) );
			Assert.AreEqual( "now", m_vm.Get( Vars.G.Label ) );
			Assert.IsFalse( m_vm.IsFiberAlive( f ) );
			Assert.AreEqual( 0, m_rt.PendingCount );
		}

		[Test]
		public void RaceCancelsPendingCommand()
		{
			AtsHandle h = m_cmd.Npcs.Add( new Npc { Name = "商人" } );
			FiberId f = Events.FireTalk( m_vm, m_prog, h, "こんにちは" );
			m_vm.Update( 0 );
			CollectionAssert.AreEqual( new[] { "商人", "こんにちは" }, m_cmd.Log );
			Assert.AreEqual( h, m_vm.Get( Vars.Tmp.Last ) );		// handle の引数と変数
			Assert.IsTrue( m_cmd.Waits.ContainsKey( "long" ) );

			m_vm.Update( 0.6f );		// wait 0.5 が先に終わり、Wait("long") は中断される
			CollectionAssert.AreEqual( new[] { "long" }, m_cmd.Cancelled );
			CollectionAssert.AreEqual( new[] { "商人", "こんにちは", "raced" }, m_cmd.Log );
			Assert.IsFalse( m_vm.IsFiberAlive( f ) );
			Assert.AreEqual( 0, m_rt.PendingCount );
		}

		[Test]
		public void AbortFiberCancelsToken()
		{
			FiberId f = Events.FireStart( m_vm, m_prog );
			m_vm.Update( 0 );
			Assert.IsTrue( m_vm.AbortFiber( f ) );
			CollectionAssert.AreEqual( new[] { "a" }, m_cmd.Cancelled );
			Assert.AreEqual( 0, m_rt.PendingCount );
			Assert.IsFalse( m_vm.AbortFiber( f ) );		// 終了済み
		}

		[UnityTest]
		public IEnumerator CompletionAfterCancelIsIgnored()
		{
			FiberId f = Events.FireStart( m_vm, m_prog );
			m_vm.Update( 0 );
			AwaitableCompletionSource s = m_cmd.Waits["a"];
			m_vm.AbortFiber( f );
			s.TrySetResult();			// キャンセル済み（TrySetCanceled 済みなので false でもよい）
			yield return Step();
			CollectionAssert.AreEqual( new[] { "start" }, m_cmd.Log );
			CollectionAssert.IsEmpty( m_errors );
		}

		[Test]
		public void DisposeVMCancelsPending()
		{
			Events.FireStart( m_vm, m_prog );
			m_vm.Update( 0 );
			m_vm.Dispose();
			CollectionAssert.AreEqual( new[] { "a" }, m_cmd.Cancelled );
			Assert.AreEqual( 0, m_rt.PendingCount );
			Assert.Throws<ObjectDisposedException>( () => m_vm.Update( 0 ) );
		}

		[Test]
		public void ExceptionInCommandFailsFiber()
		{
			FiberId f = Events.FireExplode( m_vm, m_prog );
			m_vm.Update( 0 );
			CollectionAssert.AreEqual( new[] { "before" }, m_cmd.Log );
			Assert.IsFalse( m_vm.IsFiberAlive( f ) );
			Assert.IsTrue( m_errors.Exists( e => e.Contains( "boom" ) ) );
		}

		[UnityTest]
		public IEnumerator ExceptionInAwaitableFailsFiber()
		{
			FiberId f = Events.FireStart( m_vm, m_prog );
			m_vm.Update( 0 );
			m_cmd.Waits["a"].SetException( new InvalidOperationException( "late boom" ) );
			yield return Step();
			Assert.IsFalse( m_vm.IsFiberAlive( f ) );
			CollectionAssert.AreEqual( new[] { "start" }, m_cmd.Log );
			Assert.IsTrue( m_errors.Exists( e => e.Contains( "late boom" ) ) );
			Assert.AreEqual( 0, m_rt.PendingCount );
		}

		[UnityTest]
		public IEnumerator ChannelSerializesCommands()
		{
			Events.FireChat( m_vm, m_prog, "1" );
			Events.FireChat( m_vm, m_prog, "2" );
			m_vm.Update( 0 );
			CollectionAssert.AreEqual( new[] { "speak:1" }, m_cmd.Log );		// 2 本目は channel の空き待ち

			m_cmd.Speaks[0].SetResult();
			yield return Step();
			m_vm.Update( 0 );
			CollectionAssert.AreEqual( new[] { "speak:1", "1", "speak:2" }, m_cmd.Log );

			m_cmd.Speaks[1].SetResult();
			yield return Step();
			CollectionAssert.AreEqual( new[] { "speak:1", "1", "speak:2", "2" }, m_cmd.Log );
			Assert.AreEqual( 0, m_vm.FiberCount );
		}

		[Test]
		public void BroadcastEvent()
		{
			m_vm.Attach( m_prog );
			Assert.AreEqual( 1, Events.BroadcastChat( m_vm, "x" ) );
		}

		//---------------------------------------------------------------------
		// 変数・セーブ
		//---------------------------------------------------------------------
		[Test]
		public void VariablesInitialValuesAndTypes()
		{
			Assert.AreEqual( 0, m_vm.Get( Vars.G.Counter ) );
			Assert.AreEqual( "none", m_vm.Get( Vars.G.Label ) );
			Assert.AreEqual( Mood.Angry, m_vm.Get( Vars.G.Mood ) );
			Assert.AreEqual( 0.5f, m_vm.Get( Vars.G.Ratio ) );
			Assert.IsFalse( m_vm.Get( Vars.G.Flag ) );

			m_vm.Set( Vars.G.Counter, -3 );
			m_vm.Set( Vars.G.Label, "日本語" );
			m_vm.Set( Vars.G.Mood, Mood.Calm );
			m_vm.Set( Vars.G.Ratio, 1.25f );
			m_vm.Set( Vars.G.Flag, true );
			m_vm.Set( Vars.Tmp.Last, new AtsHandle( 99 ) );
			Assert.AreEqual( -3, m_vm.Get( Vars.G.Counter ) );
			Assert.AreEqual( "日本語", m_vm.Get( Vars.G.Label ) );
			Assert.AreEqual( Mood.Calm, m_vm.Get( Vars.G.Mood ) );
			Assert.AreEqual( 1.25f, m_vm.Get( Vars.G.Ratio ) );
			Assert.IsTrue( m_vm.Get( Vars.G.Flag ) );
			Assert.AreEqual( new AtsHandle( 99 ), m_vm.Get( Vars.Tmp.Last ) );
		}

		[Test]
		public void SaveAndLoad()
		{
			m_vm.Set( Vars.G.Counter, 123 );
			m_vm.Set( Vars.G.Label, "セーブ" );
			m_vm.Set( Vars.Tmp.Last, new AtsHandle( 5 ) );		// session はセーブしない
			byte[] data = m_vm.Save();
			Assert.Greater( data.Length, 0 );

			using( ScriptVM vm2 = m_rt.CreateVM() ){
				vm2.Load( data );
				Assert.AreEqual( 123, vm2.Get( Vars.G.Counter ) );
				Assert.AreEqual( "セーブ", vm2.Get( Vars.G.Label ) );
				Assert.AreEqual( AtsHandle.None, vm2.Get( Vars.Tmp.Last ) );
			}
			var broken = new byte[ data.Length / 2 ];
			Array.Copy( data, broken, broken.Length );
			Assert.Throws<AtsException>( () => m_vm.Load( broken ) );
		}

		//---------------------------------------------------------------------
		// エラー
		//---------------------------------------------------------------------
		[Test]
		public void Errors()
		{
			var e = Assert.Throws<AtsException>( () => m_vm.FireEvent( m_prog, "NoSuchEvent" ) );
			Assert.AreEqual( AtsResult.NotFound, e.Result );

			e = Assert.Throws<AtsException>( () => m_rt.LoadProgram( new byte[] { 1, 2, 3, 4 } ) );
			Assert.AreEqual( AtsResult.BadFormat, e.Result );

			e = Assert.Throws<AtsException>( () => m_rt.RegisterQuery( "Twice", 0, _ => {} ) );
			Assert.AreEqual( AtsResult.Duplicate, e.Result );
		}

		[Test]
		public void UnregisteredCommandsAreReported()
		{
			using( var rt = new ScriptRuntime() ){
				rt.Log = ( l, m ) => {};
				var e = Assert.Throws<AtsException>( () => {
					using( ScriptProgram p = rt.LoadProgram( LoadBytes( "WrapperTest.atsb.bytes" ) ) )
					using( ScriptVM vm = rt.CreateVM() ) vm.Attach( p );
				} );
				Assert.AreEqual( AtsResult.Unresolved, e.Result );
			}
		}

		[Test]
		public void RuntimeDisposeDisposesVMs()
		{
			Events.FireStart( m_vm, m_prog );
			m_vm.Update( 0 );
			m_rt.Dispose();
			Assert.IsTrue( m_vm.IsDisposed );
			CollectionAssert.AreEqual( new[] { "a" }, m_cmd.Cancelled );
			m_prog.Dispose();		// ランタイムより後に解放しても安全
		}

		//---------------------------------------------------------------------
		// HandleTable・AtomScriptLoop
		//---------------------------------------------------------------------
		[Test]
		public void HandleTableTracksObjects()
		{
			var table = new HandleTable<GameObject>();
			var go = new GameObject( "npc" );
			AtsHandle h = table.Add( go );
			Assert.IsTrue( h.IsValid );
			Assert.AreEqual( h, table.Add( go ) );
			Assert.AreSame( go, table.Get( h ) );
			UnityEngine.Object.DestroyImmediate( go );
			Assert.IsNull( table.Get( h ) );		// 破棄済み
			Assert.IsTrue( table.Remove( h ) );
			Assert.AreEqual( 0, table.Count );
		}

		[Test]
		public void LoopInstallsPlayerLoopSystem()
		{
			AtomScriptLoop.Register( m_vm );
			try {
				Assert.AreEqual( 1, AtomScriptLoop.Count );
				Assert.IsTrue( Contains( PlayerLoop.GetCurrentPlayerLoop(), typeof( AtomScriptLoop.UpdateVMs ) ) );
				AtomScriptLoop.Register( m_vm );
				Assert.AreEqual( 1, AtomScriptLoop.Count );
				m_vm.Dispose();
				Assert.AreEqual( 0, AtomScriptLoop.Count );
			} finally {
				AtomScriptLoop.Unregister( m_vm );
			}
		}

		static bool Contains( PlayerLoopSystem s, Type t )
		{
			if( s.type == t ) return true;
			if( s.subSystemList != null ) foreach( PlayerLoopSystem c in s.subSystemList ) if( Contains( c, t ) ) return true;
			return false;
		}
	}
}
