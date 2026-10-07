//=========================================================================
//	ScriptRuntime：ランタイム（アプリ全体で 1 つ）
//	コマンド・クエリ・共有変数を登録し、プログラムの読み込みと VM の作成を行う。
//
//	ネイティブからのコールバックは [MonoPInvokeCallback] 付きの static 関数で受け、
//	user 引数の GCHandle から登録内容（Binding）を引く（IL2CPP の制約）。
//=========================================================================
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using AOT;
using AtomScript.Interop;
using UnityEngine;

namespace AtomScript
{
	// コマンドのハンドラ（即時なら Done、待機するなら AtsCall.Await の戻り値を返す）
	public delegate AtsStatus AtsCommandHandler( AtsCall call );
	// クエリのハンドラ（AtsCall.SetResult で結果を返す）
	public delegate void AtsQueryHandler( AtsCall call );

	// VM の作成オプション（0 なら既定値）
	public struct ScriptVMOptions
	{
		public uint		InstructionBudget;		// 1 回の update で実行する命令数の上限
		public uint		MaxStack;				// ファイバ 1 本のスタック要素数
		public uint		MaxCallDepth;			// 関数呼び出しの深さ
		public ulong	RandomSeed;
	}

	public sealed class ScriptRuntime : IDisposable
	{
		//---------------------------------------------------------------------
		// 登録内容（GCHandle でネイティブに渡す）
		//---------------------------------------------------------------------
		sealed class Binding
		{
			public ScriptRuntime		Runtime;
			public string				Name;
			public AtsCommandHandler	Command;
			public AtsQueryHandler		Query;
		}

		readonly RuntimeSafeHandle					m_handle = new RuntimeSafeHandle();
		readonly int								m_id;
		readonly List<GCHandle>						m_bindings = new List<GCHandle>();
		readonly Dictionary<IntPtr, ScriptVM>		m_vms = new Dictionary<IntPtr, ScriptVM>();
		readonly Dictionary<(IntPtr, ulong), PendingCall>	m_pending = new Dictionary<(IntPtr, ulong), PendingCall>();
		readonly Stack<AtsCall>						m_callPool = new Stack<AtsCall>();
		bool										m_disposed;

		// 生きているランタイム（ログの引き当てと、終了時・ドメインリロード時の破棄に使う）
		static readonly Dictionary<int, ScriptRuntime>	s_live = new Dictionary<int, ScriptRuntime>();
		static int										s_nextId;

		// ログの出力先（既定は Unity のコンソール）
		public Action<AtsLogLevel, string> Log { get; set; } = DefaultLog;

		internal IntPtr Ptr => m_disposed ? throw new ObjectDisposedException( nameof( ScriptRuntime ) ) : m_handle.DangerousGetHandle();
		internal RuntimeSafeHandle Handle => m_handle;
		public bool IsDisposed => m_disposed;

		public ScriptRuntime()
		{
			uint ver = NativeMethods.ats_get_api_version();
			if( (ver >> 16) != NativeMethods.ApiVersionMajor )
				throw new AtsException( AtsResult.Version, $"ネイティブプラグインの API バージョンが違います（{ver >> 16}.{ver & 0xffff}）" );

			m_id = ++s_nextId;
			RuntimeDesc d = default;
			d.size = (uint)Marshal.SizeOf<RuntimeDesc>();
			d.log  = s_logPtr;
			d.user = (IntPtr)m_id;
			s_live.Add( m_id, this );
			int r = NativeMethods.ats_runtime_create( ref d, out IntPtr p );
			if( r != 0 ){
				s_live.Remove( m_id );
				throw new AtsException( (AtsResult)r, "ランタイムを作成できません" );
			}
			m_handle.Set( p );
		}

		//---------------------------------------------------------------------
		// 登録（初期化後は登録内容を変えない）
		//---------------------------------------------------------------------
		// signatureHash は atsc gen が出力する値（0 ならシグネチャを検査しない）。channel は null 可
		public void RegisterCommand( string name, uint signatureHash, string channel, AtsCommandHandler handler )
		{
			if( name == null ) throw new ArgumentNullException( nameof( name ) );
			if( handler == null ) throw new ArgumentNullException( nameof( handler ) );
			GCHandle gch = GCHandle.Alloc( new Binding { Runtime = this, Name = name, Command = handler } );
			using( var n = new Utf8( name ) )
			using( var c = new Utf8( string.IsNullOrEmpty( channel ) ? null : channel ) ){
				CommandDesc d = default;
				d.size    = (uint)Marshal.SizeOf<CommandDesc>();
				d.name    = n.Ptr;
				d.sigHash = signatureHash;
				d.channel = c.Ptr;
				d.fn      = s_commandPtr;
				d.cancel  = s_cancelPtr;
				d.user    = GCHandle.ToIntPtr( gch );
				int r = NativeMethods.ats_register_command( Ptr, ref d );
				if( r != 0 ){ gch.Free(); throw Error( r, $"コマンド '{name}' を登録できません" ); }
			}
			m_bindings.Add( gch );
		}

		public void RegisterQuery( string name, uint signatureHash, AtsQueryHandler handler )
		{
			if( name == null ) throw new ArgumentNullException( nameof( name ) );
			if( handler == null ) throw new ArgumentNullException( nameof( handler ) );
			GCHandle gch = GCHandle.Alloc( new Binding { Runtime = this, Name = name, Query = handler } );
			using( var n = new Utf8( name ) ){
				QueryDesc d = default;
				d.size    = (uint)Marshal.SizeOf<QueryDesc>();
				d.name    = n.Ptr;
				d.sigHash = signatureHash;
				d.fn      = s_queryPtr;
				d.user    = GCHandle.ToIntPtr( gch );
				int r = NativeMethods.ats_register_query( Ptr, ref d );
				if( r != 0 ){ gch.Free(); throw Error( r, $"クエリ '{name}' を登録できません" ); }
			}
			m_bindings.Add( gch );
		}

		// 共有変数の定義（name は "bank.name"。init が Void なら型の既定値）
		public void DefineVar( uint id, string name, AtsType type, AtsVarScope scope, AtsValue init )
		{
			NativeValue v = init.ToNative( out IntPtr str );
			try {
				using( var n = new Utf8( name ) ){
					VarDesc d = default;
					d.size  = (uint)Marshal.SizeOf<VarDesc>();
					d.id    = id;
					d.name  = n.Ptr;
					d.type  = (uint)type;
					d.scope = (uint)scope;
					d.init  = v;
					int r = NativeMethods.ats_define_var( Ptr, ref d );
					if( r != 0 ) throw Error( r, $"変数 '{name}' を定義できません" );
				}
			} finally {
				if( str != IntPtr.Zero ) Marshal.FreeCoTaskMem( str );
			}
		}

		//---------------------------------------------------------------------
		// プログラム・VM
		//---------------------------------------------------------------------
		// .atsb のバイト列を読み込む（内部にコピーされるので bytes は呼び出し後に捨ててよい）
		public ScriptProgram LoadProgram( byte[] bytes )
		{
			if( bytes == null ) throw new ArgumentNullException( nameof( bytes ) );
			int r = NativeMethods.ats_program_load( Ptr, bytes, (UIntPtr)bytes.Length, out IntPtr p );
			if( r != 0 ) throw Error( r, "プログラムを読み込めません" );
			return new ScriptProgram( this, p );
		}

		// インポートした .ats を読み込む
		public ScriptProgram LoadProgram( AtsScriptAsset asset )
		{
			if( asset == null ) throw new ArgumentNullException( nameof( asset ) );
			return LoadProgram( asset.Bytecode );
		}

		public ScriptVM CreateVM() => CreateVM( default );

		public ScriptVM CreateVM( ScriptVMOptions options )
		{
			VmDesc d = default;
			d.size              = (uint)Marshal.SizeOf<VmDesc>();
			d.instructionBudget = options.InstructionBudget;
			d.maxStack          = options.MaxStack;
			d.maxCallDepth      = options.MaxCallDepth;
			d.randomSeed        = options.RandomSeed;
			int r = NativeMethods.ats_vm_create( Ptr, ref d, out IntPtr p );
			if( r != 0 ) throw Error( r, "VM を作成できません" );
			var vm = new ScriptVM( this, p );
			m_vms.Add( p, vm );
			return vm;
		}

		internal void OnVMDisposed( IntPtr p ) => m_vms.Remove( p );
		internal ScriptVM FindVM( IntPtr p ) => m_vms.TryGetValue( p, out ScriptVM vm ) ? vm : null;

		//---------------------------------------------------------------------
		// 破棄（VM を先に破棄する。待機中のコマンドにはキャンセルが届く）
		//---------------------------------------------------------------------
		public void Dispose()
		{
			if( m_disposed ) return;
			foreach( ScriptVM vm in new List<ScriptVM>( m_vms.Values ) ) vm.Dispose();
			m_vms.Clear();
			m_pending.Clear();
			m_disposed = true;
			// プログラムが残っていれば、ネイティブのランタイムはその解放まで残る（SafeHandle の参照カウント）
			m_handle.Dispose();
			foreach( GCHandle h in m_bindings ) h.Free();
			m_bindings.Clear();
			s_live.Remove( m_id );
		}

		// 生きているランタイムをすべて破棄する（アプリ終了・ドメインリロード）
		public static void DisposeAll()
		{
			foreach( ScriptRuntime rt in new List<ScriptRuntime>( s_live.Values ) ) rt.Dispose();
		}

#if UNITY_EDITOR
		[UnityEditor.InitializeOnLoadMethod]
		static void HookEditor()
		{
			UnityEditor.AssemblyReloadEvents.beforeAssemblyReload += DisposeAll;
		}
#endif

		[RuntimeInitializeOnLoadMethod( RuntimeInitializeLoadType.SubsystemRegistration )]
		static void HookPlayer()
		{
			Application.quitting -= DisposeAll;
			Application.quitting += DisposeAll;
		}

		//---------------------------------------------------------------------
		// エラー
		//---------------------------------------------------------------------
		internal AtsException Error( int r, string what )
		{
			string detail = m_disposed ? null : Utf8.ToManaged( NativeMethods.ats_runtime_last_error( m_handle.DangerousGetHandle() ) );
			return new AtsException( (AtsResult)r, string.IsNullOrEmpty( detail ) ? what : $"{what}: {detail}" );
		}

		internal void ReportException( string name, Exception e )
		{
			Log?.Invoke( AtsLogLevel.Error, $"[AtomScript] '{name}' のハンドラで例外: {e}" );
		}

		static void DefaultLog( AtsLogLevel level, string message )
		{
			switch( level ){
			case AtsLogLevel.Error:		Debug.LogError( message ); break;
			case AtsLogLevel.Warning:	Debug.LogWarning( message ); break;
			default:					Debug.Log( message ); break;
			}
		}

		//---------------------------------------------------------------------
		// 待機中の呼び出し
		//---------------------------------------------------------------------
		internal PendingCall AddPending( IntPtr vm, ulong token )
		{
			var p = new PendingCall( this, vm, token );
			m_pending[ (vm, token) ] = p;
			return p;
		}

		internal void RemovePending( PendingCall p )
		{
			if( m_pending.TryGetValue( (p.VM, p.Token), out PendingCall cur ) && cur == p ) m_pending.Remove( (p.VM, p.Token) );
		}

		// 完了待ちの待機ありコマンドの数（リークの確認用）
		public int PendingCount => m_pending.Count;

		AtsCall RentCall( IntPtr call ) { AtsCall c = m_callPool.Count > 0 ? m_callPool.Pop() : new AtsCall(); c.Begin( this, call ); return c; }
		void ReturnCall( AtsCall c ) { c.End(); m_callPool.Push( c ); }

		//---------------------------------------------------------------------
		// コールバック（ネイティブから呼ばれる。例外を外に出さない）
		//---------------------------------------------------------------------
		static readonly LogFn		s_logFn		= OnLog;
		static readonly CommandFn	s_commandFn	= OnCommand;
		static readonly CancelFn	s_cancelFn	= OnCancel;
		static readonly QueryFn		s_queryFn	= OnQuery;
		static readonly IntPtr		s_logPtr		= Marshal.GetFunctionPointerForDelegate( s_logFn );
		static readonly IntPtr		s_commandPtr	= Marshal.GetFunctionPointerForDelegate( s_commandFn );
		static readonly IntPtr		s_cancelPtr		= Marshal.GetFunctionPointerForDelegate( s_cancelFn );
		static readonly IntPtr		s_queryPtr		= Marshal.GetFunctionPointerForDelegate( s_queryFn );

		[MonoPInvokeCallback( typeof( LogFn ) )]
		static void OnLog( int level, IntPtr message, IntPtr user )
		{
			try {
				if( s_live.TryGetValue( (int)user, out ScriptRuntime rt ) )
					rt.Log?.Invoke( (AtsLogLevel)level, "[AtomScript] " + Utf8.ToManaged( message ) );
			} catch( Exception e ){
				Debug.LogException( e );
			}
		}

		[MonoPInvokeCallback( typeof( CommandFn ) )]
		static int OnCommand( IntPtr call, IntPtr user )
		{
			var b = (Binding)GCHandle.FromIntPtr( user ).Target;
			AtsCall c = b.Runtime.RentCall( call );
			AtsStatus st = AtsStatus.Fail;
			try {
				st = b.Command( c );
			} catch( Exception e ){
				b.Runtime.ReportException( b.Name, e );
				st = AtsStatus.Fail;
			} finally {
				c.Settle( st );
				b.Runtime.ReturnCall( c );
			}
			return (int)st;
		}

		[MonoPInvokeCallback( typeof( CancelFn ) )]
		static void OnCancel( IntPtr vm, ulong token, IntPtr user )
		{
			var b = (Binding)GCHandle.FromIntPtr( user ).Target;
			try {
				if( b.Runtime.m_pending.TryGetValue( (vm, token), out PendingCall p ) ) p.Cancel();
			} catch( Exception e ){
				b.Runtime.ReportException( b.Name, e );
			}
		}

		[MonoPInvokeCallback( typeof( QueryFn ) )]
		static int OnQuery( IntPtr call, IntPtr user )
		{
			var b = (Binding)GCHandle.FromIntPtr( user ).Target;
			AtsCall c = b.Runtime.RentCall( call );
			try {
				b.Query( c );
				return 0;
			} catch( AtsException e ){
				b.Runtime.ReportException( b.Name, e );
				return (int)e.Result;
			} catch( Exception e ){
				b.Runtime.ReportException( b.Name, e );
				return (int)AtsResult.InvalidArg;
			} finally {
				b.Runtime.ReturnCall( c );
			}
		}
	}
}
